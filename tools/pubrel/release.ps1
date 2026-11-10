#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^mas[1-9][0-9]*$')][string]$Build,
    [Parameter(Mandatory)][string]$Sdk,
    [ValidateRange(0, 999)][int]$Patch = 0,
    # Paths not given here come from the local configuration (tools\local.ps1, keys in tools\local.example.psd1);
    # one that is in neither place stops the run. AOSP emulator source tree (EmuSrc).
    [string]$EmuSrc,
    # default <TempRoot>\pubrel
    [string]$OutputRoot,
    [string]$OutputName,
    # source baseline: unchanged source archives are reused after checking their frozen hashes (Baseline)
    [string]$Baseline,
    # reviewed 说明.txt and LICENSES\ (LicensePackage); preflight refuses a package without LICENSES\AGPL-3.0.txt or
    # with another text in it (SHA-256 after CRLF -> LF, release_tools.py AGPL_SHA256)
    [string]$Package,
    # ModAVD release checkout: the bundled scripts (package/) and the bridge source (bridge/) are taken from its HEAD,
    # and the patches, manifests and release notes are written into it. Default: the checkout these tools are in.
    [string]$Repository,
    # ModAVDManager-<version>-win-x64.zip: ModAVD.exe and tools\modavd.exe come from it. From the manager's GitHub
    # Release, or before that release exists built from its release branch (build.ps1 and mkzip there; same bytes).
    [Parameter(Mandatory)][string]$Manager,
    # checkouts of the public repositories, fetched from upstream with tags: the manager ZIP and the IME APK are checked
    # against the release checksums at their tags, and the source bundle archives those commits (ManagerRepo / ImeRepo;
    # ExtrasRepo below). A manager or extras version that has no tag yet is taken at the head of upstream's
    # release/0.<N> (N from -Build), which already carries its release/<version>.SHA256SUMS.txt: the package can be
    # built and tested before the components are tagged, and the serial release tags exactly those commits.
    # -DraftRelease refuses a package with untagged components.
    [string]$ManagerRepo,
    [string]$ImeRepo,
    # merged incoming\<topic> held for a later release; every other merged topic must be in the new series
    [string[]]$KeepIncoming = @(),
    # desktop APK, shipped as components\<file name> (LauncherApk)
    [string]$Apk,
    # host input method APK, shipped as components\modavd-ime.apk; '' = not shipped. Every APK in the package is pinned
    # in config\components.SHA256SUMS.txt: an unpinned one is refused, a pinned one that is missing fails the release.
    [string]$ImeApk = '',
    # ModAVDExtras' capture library modavd_extras.dll, shipped as extras\modavd_extras.dll; '' = not shipped. Pinned like
    # the APKs (config\components.SHA256SUMS.txt), and tag v<EXTRAS_VERSION> of -ExtrasRepo (local configuration ExtrasRepo;
    # before the tag, the head of its release/0.<N>) must list it in release/<version>.SHA256SUMS.txt; the source bundle
    # archives that commit. The DLL is the one its CI built: another MSVC gives other bytes.
    [string]$ExtrasDll = '',
    [string]$ExtrasRepo,
    [string]$Changes = (Join-Path $PSScriptRoot 'config\changes.md'),
    [string]$Changelog = (Join-Path $PSScriptRoot 'config\changelog.json'),
    [string]$Snapshot,
    # privacy-private-rules.json, kept outside this repository (PrivateRules, else MODAVD_PRIVATE_RULES): the release
    # privacy scan refuses to run without it
    [string]$PrivateRules,
    # step 8 skips files that the last scan found clean and that have not changed since (same SHA-256 at the same path,
    # same scanner and rules, every exception they need still in the allowlist). The cache is local, outside the
    # repository and the output: PrivacyCache in the local configuration, else <OutputRoot>\privacy-cache.json.
    # -NoPrivacyCache scans everything. With -DraftRelease (the run that uploads) step 8 does not read the cache: it
    # scans everything and only writes the cache.
    [string]$PrivacyCache,
    [switch]$NoPrivacyCache,
    # free space required before writing, GiB (MinFreeGB; 120 when neither)
    [int]$MinFreeGB,
    [string]$Python,
    [string]$SevenZip,
    [switch]$Force,
    [switch]$DraftRelease
)
$ErrorActionPreference = 'Stop'
[cultureinfo]::CurrentCulture = [cultureinfo]::InvariantCulture
[cultureinfo]::CurrentUICulture = [cultureinfo]::InvariantCulture
$PSNativeCommandUseErrorActionPreference = $false
. (Join-Path $PSScriptRoot '..\local.ps1')
# pwsh -File 把 `-KeepIncoming a,b` 当成一个字符串
$KeepIncoming = @($KeepIncoming | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
# 公开版本号 0.<N>.<P>：0 是内测阶段，N 是构建号，P 是不重编、只重新打包的次数（RELEASING.md「版本号」）
$number = [int]$Build.Substring(3)
$version = "0.$number.$Patch"
$defaultName = if ($Patch) { "$Build-p$Patch" } else { $Build }
if (-not $OutputName) { $OutputName = $defaultName }
if ($OutputName -notin @($defaultName, "$defaultName-check")) { throw "OutputName 只能是 $defaultName 或 $defaultName-check" }

$EmuSrc = Use-Local EmuSrc $EmuSrc '-EmuSrc'
if (-not $OutputRoot) { $OutputRoot = Join-Path (Use-Local TempRoot $null '-OutputRoot') 'pubrel' }
$Baseline = Use-Local Baseline $Baseline '-Baseline'
$Package = Use-Local LicensePackage $Package '-Package'
$Apk = Use-Local LauncherApk $Apk '-Apk'
$ManagerRepo = Use-Local ManagerRepo $ManagerRepo '-ManagerRepo'
$ImeRepo = Use-Local ImeRepo $ImeRepo '-ImeRepo' -Optional:(-not $ImeApk)
$ExtrasRepo = Use-Local ExtrasRepo $ExtrasRepo '-ExtrasRepo' -Optional:(-not $ExtrasDll)
if (-not $Repository) { $Repository = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
if (-not $MinFreeGB) { $MinFreeGB = [int](Use-Local MinFreeGB $null '-MinFreeGB' -Optional) }
if (-not $MinFreeGB) { $MinFreeGB = 120 }
# 私有规则在第 8 步才用，先确认有，免得跑完前七步才停
if (-not $PrivateRules) { $PrivateRules = Use-Local PrivateRules $null '-PrivateRules' -Optional }
if (-not $PrivateRules) { $PrivateRules = $env:MODAVD_PRIVATE_RULES }
if (-not $PrivateRules -or -not (Test-Path -LiteralPath $PrivateRules -PathType Leaf)) {
    throw "找不到私有隐私规则（-PrivateRules、本机配置 PrivateRules 或环境变量 MODAVD_PRIVATE_RULES）：$PrivateRules"
}

$root = [IO.Path]::GetFullPath($OutputRoot).TrimEnd('\', '/')
$out = [IO.Path]::GetFullPath((Join-Path $root $OutputName))
if ((Split-Path $out -Parent) -ne $root) { throw '输出必须是 OutputRoot 的直接子目录' }
# 源码、备份、生产目录（本机配置 ProtectedRoots）和源码树、SDK 里都不许写；发版克隆由 release_tools.py 用同一份清单查
$protect = @(@(Use-Local ProtectedRoots $null '' -Optional) + @($EmuSrc, $Sdk) | Where-Object { $_ })
$hit = Test-Inside $root ($protect + @($Repository))
if ($hit) { throw "OutputRoot 必须使用独立的临时数据目录，不能在源码、发版克隆或生产目录里：$root 在 $hit 下" }
$protectArgs = @($protect | ForEach-Object { '--protect', $_ })
if ($NoPrivacyCache) { $PrivacyCache = '' }
else {
    if (-not $PrivacyCache) { $PrivacyCache = Use-Local PrivacyCache $null '' -Optional }
    if (-not $PrivacyCache) { $PrivacyCache = Join-Path $root 'privacy-cache.json' }
    if (Test-Inside $PrivacyCache @($out, $Repository)) { throw "隐私扫描缓存不能放在输出目录或发版克隆里：$PrivacyCache" }
}
Write-Host "隐私扫描缓存：$(if (-not $PrivacyCache) { '不用（-NoPrivacyCache），第 8 步全部重扫' } elseif ($DraftRelease) { "$PrivacyCache（-DraftRelease：第 8 步不读，全部重扫，扫完只写）" } else { $PrivacyCache })"

if (-not $Python) { $Python = $env:MODAVD_PYTHON }
if (-not $Python) { $Python = Use-Local Python $null '-Python' -Optional }
if (-not $Python) { $Python = (Get-Command python.exe -ErrorAction SilentlyContinue).Source }
if (-not $Python) { throw '找不到 Python 3.12：给 -Python，或者设 MODAVD_PYTHON、本机配置 Python' }
if (-not $SevenZip) { $SevenZip = Use-Local SevenZip $null '-SevenZip' -Optional }
if (-not $SevenZip) { $SevenZip = (Get-Command 7z.exe -ErrorAction SilentlyContinue).Source }
if (-not $SevenZip -and (Test-Path -LiteralPath "$env:ProgramFiles\7-Zip\7z.exe")) { $SevenZip = "$env:ProgramFiles\7-Zip\7z.exe" }
if (-not $SevenZip) { throw '找不到 7-Zip：给 -SevenZip，或者写进本机配置 SevenZip' }

$helper = Join-Path $PSScriptRoot 'release_tools.py'
function Run-Python([string[]]$Arguments) {
    & $Python -I $helper @Arguments
    if ($LASTEXITCODE -ne 0) { throw "release_tools.py 返回 $LASTEXITCODE" }
}
function Step([int]$StepNumber, [string]$Title, [scriptblock]$Action) {
    Write-Host "[$StepNumber/8] $Title"
    try { & $Action } catch { throw "步骤 $StepNumber（$Title）失败：$($_.Exception.Message)" }
}
# preflight validates the SDK, version, release texts, immutable baseline assets, the license package (AGPL-3.0 full text),
# disk budget and snapshot before any deletion.
if (-not $Snapshot) { $Snapshot = Join-Path $Sdk 'build-info.json' }
$texts = @('--patch', $Patch, '--changes', $Changes, '--changelog', $Changelog)
$componentArgs = @(if ($ImeApk) { '--ime-apk', $ImeApk, '--ime-repo', $ImeRepo })
if ($ExtrasDll) { $componentArgs += @('--extras-dll', $ExtrasDll, '--extras-repo', $ExtrasRepo) }
# 还没打 tag 的管理器、截图库取各自仓库 upstream 的发版分支头（release_tools.py 的 component_ref）
$public = @('--repository', $Repository, '--manager', $Manager, '--manager-repo', $ManagerRepo,
    '--release-branch', "release/0.$number") + $protectArgs
# 建草稿的那一轮只收已经打了 tag 的组件
$tagArgs = @(if ($DraftRelease) { '--require-tags' })
Run-Python (@('preflight') + $tagArgs + @('--build', $Build, '--sdk', $Sdk, '--snapshot', $Snapshot, '--min-free-gb', $MinFreeGB,
    '--baseline', $Baseline, '--package', $Package, '--apk', $Apk, '--output', $out) + $public + $componentArgs + $texts)
if (Test-Path -LiteralPath $out) {
    if (-not $Force) { throw "输出已存在：$out（-Force 只清理本脚本创建并标记的同名输出目录）" }
    # Reject junctions/symlinks at every level; validate the ownership marker and exact path before Remove-Item.
    Run-Python @('check-owned', '--output', $out, '--build', $Build)
    Remove-Item -LiteralPath $out -Recurse -Force
}
New-Item -ItemType Directory -Path $out | Out-Null
Run-Python @('own', '--output', $out, '--build', $Build)
$common = @('--output', $out)
Step 1 '组装 release 目录并写清单（管理器取它的 Release 包，随包脚本取 ModAVD 的 package/）' {
    Run-Python (@('assemble') + $tagArgs + $common + $texts + $public + @('--build', $Build, '--sdk', $Sdk, '--snapshot', $Snapshot,
        '--package', $Package, '--apk', $Apk) + $componentArgs)
}
# 主包带平台（以后可能有 linux-x64），源码包不带；和 release_tools.py 的 PLATFORM 一致
$name = "ModAVD-$version-win-x64"
$zip = Join-Path $out "$name.zip"
$sourcesZip = Join-Path $out "ModAVD-$version-sources.zip"
Step 2 '打主包 ZIP（没有顶层目录，UTF-8 文件名）' {
    # 包根就是 ModAVD 根目录：Mirror 酱按路径比文件，带版本号的顶层目录会让增量等于全量。
    # 「全部解压缩」/ 7-Zip「解压到 <包名>\」照样会建同名文件夹。
    Push-Location (Join-Path $out "release\$name")
    try {
        & $SevenZip a -tzip -mcu=on -mx=6 -mmt=8 $zip '*'
        if ($LASTEXITCODE -ne 0) { throw "7-Zip 返回 $LASTEXITCODE（主包）" }
    } finally { Pop-Location }
}
Step 3 'verify-zip：内容、条目路径、UTF-8 标志、排除文件、清单' { Run-Python (@('verify-zip') + $common) }
Step 4 '导出补丁并逐提交回放比对树和作者，核对并移除已收录的 incoming' {
    Run-Python (@('patches', '--emu-src', $EmuSrc) + $common)
    Run-Python (@('replay', '--emu-src', $EmuSrc) + $common)
    $keepArgs = @($KeepIncoming | ForEach-Object { '--keep-incoming', $_ })
    Run-Python (@('incoming', '--repository', $Repository) + $protectArgs + $common + $keepArgs)
}
Step 5 '复用源码、打不压缩的源码总包、同步仓库清单' {
    Run-Python (@('sources', '--emu-src', $EmuSrc, '--baseline', $Baseline, '--min-free-gb', $MinFreeGB) + $common)
    Run-Python (@('bundle', '--sevenzip', $SevenZip, '--repository', $Repository) + $protectArgs + $common)
}
Step 6 '生成两份 SHA256SUMS' { Run-Python (@('sums') + $common) }
Step 7 '从模板生成 Release 说明并校验公告、表格' { Run-Python (@('notes') + $common + $texts) }
Step 8 '隐私扫描（含全部源码包成员，加上仓库外的私有规则；上次扫过干净、没变的文件按缓存跳过）' {
    $cacheArgs = if ($PrivacyCache) { @('--privacy-cache', $PrivacyCache) } else { @() }
    # 真要上传的这一轮不信缓存：照格式写的「干净」记录（手改的，或者缓存逻辑以后出了错）不能决定发出去的东西
    if ($DraftRelease -and $PrivacyCache) { $cacheArgs += '--privacy-cache-write-only' }
    Run-Python (@('privacy-release', '--private-rules', $PrivateRules, '--repository', $Repository) + $cacheArgs + $common)
}
if ($DraftRelease) {
    Write-Host '[9] 创建 GitHub 草稿（不会发布）'
    $upload = @($zip, $sourcesZip, (Join-Path $out 'SHA256SUMS.txt'))
    & gh release create "v$version" --draft --repo ModAVD/ModAVD --target "release/0.$number" --title "ModAVD mas-$number（$version）" --notes-file (Join-Path $out 'release-notes.md') @upload
    if ($LASTEXITCODE -ne 0) { throw "步骤 9（创建草稿）失败：gh 返回 $LASTEXITCODE；未自动重试，请检查草稿状态" }
}
Write-Host "完成：$out（v$version）"
