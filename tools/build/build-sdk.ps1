#requires -Version 7.0
[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)][ValidatePattern('^mas[1-9][0-9]*$')][string]$Build,
    # 没给的路径从本机配置取（tools\local.ps1，键名见 tools\local.example.psd1），配置里也没有就报错。
    # AOSP 模拟器源码树（EmuSrc）；产物装到它下面的 sdk-<Build>
    [string]$EmuSrc,
    [ValidateRange(1,16)][int]$Jobs = 12,
    # 默认取环境变量 MODAVD_PYTHON、本机配置 Python，都没有就是 PATH 里的 python
    [string]$Python,
    # 默认 <TempRoot>\build-sdk
    [string]$LogRoot,
    # 编之前要求源码树所在的盘至少空出多少 GiB（MinFreeGB；都没有就是 120）
    [int]$MinFreeGB,
    # arm64 执行桥的三个载荷文件（tools\gate\）：一个已冻结的目录，里面就是那三个文件。
    # 不给就用 stage-payload.ps1 的默认产物位置。它们不是 AOSP 构建产物，所以编完单独放进 SDK 树。
    [string]$GatePayload
)
$ErrorActionPreference = 'Stop'
[cultureinfo]::CurrentCulture = [cultureinfo]::InvariantCulture
[cultureinfo]::CurrentUICulture = [cultureinfo]::InvariantCulture
$PSNativeCommandUseErrorActionPreference = $false
. (Join-Path $PSScriptRoot '..\local.ps1')
$EmuSrc = Use-Local EmuSrc $EmuSrc '-EmuSrc'
if (-not $LogRoot) { $LogRoot = Join-Path (Use-Local TempRoot $null '-LogRoot') 'build-sdk' }
if (-not $Python) { $Python = $env:MODAVD_PYTHON }
if (-not $Python) { $Python = Use-Local Python $null '-Python' -Optional }
if (-not $Python) { $Python = 'python' }
if (-not $MinFreeGB) { $MinFreeGB = [int](Use-Local MinFreeGB $null '-MinFreeGB' -Optional) }
if (-not $MinFreeGB) { $MinFreeGB = 120 }
$sourceRoot = [IO.Path]::GetFullPath($EmuSrc)
$target = Join-Path $sourceRoot "sdk-$Build"
if (Test-Path -LiteralPath $target) { throw "目标已存在，拒绝覆盖：$target" }
$qemu = Join-Path $sourceRoot 'external\qemu'
$distribution = Join-Path $sourceRoot 'objs\distribution\emulator'
$helper = Join-Path $PSScriptRoot '..\pubrel\release_tools.py'
$logDir = Join-Path $LogRoot ("$Build-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
$snapshot = Join-Path $logDir 'build-info.json'
$disabled = @('clean', 'removeleftover', 'prebuilts', 'ctest', 'emugentest', 'genentriestest',
    'coveragereport', 'packagesamples', 'zipintegrationtests', 'integrationtest')
$buildArgs = @('android\build\python\cmake.py', '--out', (Join-Path $sourceRoot 'objs'), '--dist', (Join-Path $sourceRoot 'dist'))
foreach ($task in $disabled) { $buildArgs += @('--task-disable', $task) }
$stageGate = Join-Path $PSScriptRoot '..\gate\stage-payload.ps1'
$gateArgs = if ($GatePayload) { @('-Payload', $GatePayload) } else { @() }
Write-Host "并行上限：$Jobs；工作目录：$qemu"
Write-Host "$Python -I $helper snapshot --build $Build --emu-src $sourceRoot --snapshot $snapshot"
Write-Host "CMAKE_BUILD_PARALLEL_LEVEL=$Jobs"
Write-Host "$Python $($buildArgs -join ' ')"
Write-Host "robocopy $distribution $target\emulator /E /COPY:DAT /DCOPY:DAT /R:1 /W:1 /XJ"
Write-Host "写 $target\emulator\source.properties：Pkg.BuildId=mas-$($Build.Substring(3))"
Write-Host "放 arm64 执行桥载荷：$stageGate -Sdk $target $($gateArgs -join ' ')"
Write-Host "保存源码提交快照：$target\build-info.json；日志：$logDir"
# 载荷核对只读，放在 ShouldProcess 之前：-WhatIf 也要验一遍，免得编一个多小时之后才发现
# 载荷没编或哈希没重新固定。对不上就直接 throw（见 tools\gate\README.md）。
& $stageGate -CheckOnly @gateArgs
if (-not $PSCmdlet.ShouldProcess($target, '增量编译并安装独立 SDK')) { return }
# 源码树可能是目录联接：看它真正所在的那个盘
$real = (Get-Item -LiteralPath $sourceRoot).ResolveLinkTarget($true)
$drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($(if ($real) { $real.FullName } else { $sourceRoot })))
if ($drive.AvailableFreeSpace -lt $MinFreeGB * 1GB) { throw "$($drive.Name) 至少需 $MinFreeGB GiB 可用空间（-MinFreeGB），现在 $([int]($drive.AvailableFreeSpace / 1GB)) GiB" }
New-Item -ItemType Directory -Path $logDir | Out-Null
& $Python -I $helper snapshot --build $Build --emu-src $sourceRoot --snapshot $snapshot
if ($LASTEXITCODE) { throw '读取源码提交快照失败' }
$oldUtf8 = $env:PYTHONUTF8
$oldParallel = $env:CMAKE_BUILD_PARALLEL_LEVEL
$oldPath = $env:PATH
$oldNinja = $env:NINJA_STATUS
try {
    $env:PYTHONUTF8 = '1'
    $env:CMAKE_BUILD_PARALLEL_LEVEL = [string]$Jobs
    $env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:PATH"
    $env:NINJA_STATUS = '[%f/%t] '
    Push-Location $qemu
    try {
        & $Python @buildArgs *>&1 | Tee-Object -FilePath (Join-Path $logDir 'build.log')
        if ($LASTEXITCODE) { throw "编译失败，退出码 $LASTEXITCODE；见 $logDir\build.log" }
    } finally { Pop-Location }
    if (-not (Test-Path -LiteralPath (Join-Path $distribution 'emulator.exe'))) { throw "缺少构建产物：$distribution" }
    # Capture again to detect changed commits/files during the long build, before labeling any binary.
    $after = Join-Path $logDir 'build-info-after.json'
    & $Python -I $helper snapshot --build $Build --emu-src $sourceRoot --snapshot $after
    if ($LASTEXITCODE -or (Get-FileHash $snapshot).Hash -ne (Get-FileHash $after).Hash) { throw '编译期间源码改变；拒绝标记 SDK' }
    if (Test-Path -LiteralPath $target) { throw "编译期间目标已被创建：$target" }
    New-Item -ItemType Directory -Path $target | Out-Null
    & robocopy $distribution (Join-Path $target 'emulator') /E /COPY:DAT /DCOPY:DAT /R:1 /W:1 /XJ /NP /NFL /NDL
    if ($LASTEXITCODE -ge 8) { throw "robocopy 失败，退出码 $LASTEXITCODE；保留目标以便排查" }
    $props = Join-Path $target 'emulator\source.properties'
    $text = [IO.File]::ReadAllText($props)
    $id = "Pkg.BuildId=mas-$($Build.Substring(3))"
    if ($text -match '(?m)^Pkg\.BuildId=.*$') { $text = [regex]::Replace($text, '(?m)^Pkg\.BuildId=[^\r\n]*', $id) }
    else { $text = $text.TrimEnd() + "`r`n$id`r`n" }
    [IO.File]::WriteAllText($props, $text, [Text.UTF8Encoding]::new($false))
    # arm64 执行桥的三个载荷文件（emulator\resources\modavd\）。不是 AOSP 产物，所以在这里单独放进去。
    # 发版 assemble 对 config\sdk-inventory.csv 是全等判定，少一个就停，所以漏了不会静默发出去。
    & $stageGate -Sdk $target @gateArgs
    Copy-Item -LiteralPath $snapshot -Destination (Join-Path $target 'build-info.json')
    Write-Host "SDK 完成：$target"
} finally {
    $env:PYTHONUTF8 = $oldUtf8
    $env:CMAKE_BUILD_PARALLEL_LEVEL = $oldParallel
    $env:PATH = $oldPath
    $env:NINJA_STATUS = $oldNinja
}
