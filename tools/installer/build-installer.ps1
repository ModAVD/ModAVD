#Requires -Version 7.0
<#
.SYNOPSIS
  把发版 zip（或它解压出来的目录）编成 Inno Setup 安装程序 ModAVD-<版本>-win-x64-setup.exe。

.DESCRIPTION
  只读输入：先核对载荷是一个干净的发版包（有 ModAVD.exe、modavd-manifest.json、sdk\emulator，没有实例和用户数据，
  清单里的版本和 -Version 一致），再调 ISCC 编 modavd.iss。不联网，不改仓库里的文件。

.PARAMETER Source
  发版 zip（ModAVD-<版本>-win-x64.zip），或已经解压好的目录（顶层就是 ModAVD.exe）。

.PARAMETER Version
  版本号，比如 0.43.0（不带 v）。

.PARAMETER OutDir
  安装程序放哪。

.PARAMETER Work
  Source 是 zip 时解压到哪：必须是不存在或空的目录。Source 是目录时不用。

.PARAMETER Iscc
  ISCC.exe（Inno Setup 6）的路径。不给就找 PATH 里的 ISCC.exe。

.PARAMETER SelfTest
  编自检版（不带载荷，文件名后加 -selftest）：用 /VERYSILENT /LOG=<文件> 运行，日志里逐条写 SELFTEST PASS / FAIL。

.EXAMPLE
  pwsh -NoProfile -File tools/installer/build-installer.ps1 -Source <目录>\ModAVD-0.43.0-win-x64.zip `
    -Work <空目录> -Version 0.43.0 -OutDir <输出目录> -Iscc <Inno Setup 目录>\ISCC.exe
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][string]$OutDir,
    [string]$Work,
    [string]$Iscc,
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Stop-Build([string]$message) {
    throw "build-installer: $message"
}

if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    Stop-Build "版本号要写成 0.43.0 这样（不带 v），收到「$Version」"
}

$scriptRoot = $PSScriptRoot
$issPath = Join-Path $scriptRoot 'modavd.iss'

# ---- ISCC ----
if (-not $Iscc) {
    $found = Get-Command ISCC.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $found) { Stop-Build '找不到 ISCC.exe：用 -Iscc 指过去' }
    $Iscc = $found.Source
}
if (-not (Test-Path -LiteralPath $Iscc -PathType Leaf)) { Stop-Build "ISCC 不存在：$Iscc" }
$Iscc = (Resolve-Path -LiteralPath $Iscc).Path

# ---- 载荷 ----
$payloadDir = $null
if (-not $SelfTest) {
    if (-not (Test-Path -LiteralPath $Source)) { Stop-Build "找不到 $Source" }
    if (Test-Path -LiteralPath $Source -PathType Leaf) {
        if (-not $Work) { Stop-Build 'Source 是 zip 时要给 -Work（解压目录，不存在或为空）' }
        if ((Test-Path -LiteralPath $Work) -and (Get-ChildItem -LiteralPath $Work -Force | Select-Object -First 1)) {
            Stop-Build "解压目录不是空的：$Work"
        }
        New-Item -ItemType Directory -Force -Path $Work | Out-Null
        Write-Host "解压 $Source -> $Work"
        Expand-Archive -LiteralPath $Source -DestinationPath $Work
        $payloadDir = (Resolve-Path -LiteralPath $Work).Path
    }
    else {
        $payloadDir = (Resolve-Path -LiteralPath $Source).Path
    }

    foreach ($required in 'ModAVD.exe', 'modavd-manifest.json', 'sdk\emulator') {
        if (-not (Test-Path -LiteralPath (Join-Path $payloadDir $required))) {
            Stop-Build "载荷里没有 $required：$payloadDir 不像发版包的顶层（zip 顶层就该是 ModAVD.exe）"
        }
    }
    # 实例和用户数据绝不能进安装程序（这些名字和管理器更新时不许包里出现的一致）。
    foreach ($forbidden in 'avd', 'downloads', 'logs', '.update', 'shader-cache', 'guest-patches', 'mas-avd.json') {
        if (Test-Path -LiteralPath (Join-Path $payloadDir $forbidden)) {
            Stop-Build "载荷里有用户数据 $forbidden，不是干净的发版包：$payloadDir"
        }
    }
    $sdkExtra = Get-ChildItem -LiteralPath (Join-Path $payloadDir 'sdk') -Force | Where-Object Name -ne 'emulator'
    if ($sdkExtra) {
        Stop-Build "载荷的 sdk\ 下只该有 emulator，多了：$(($sdkExtra.Name) -join ', ')（adb、系统镜像由管理器下载，不随包发）"
    }
    $manifest = Get-Content -LiteralPath (Join-Path $payloadDir 'modavd-manifest.json') -Raw | ConvertFrom-Json
    if ($manifest.build -ne $Version -or $manifest.version -ne "v$Version") {
        Stop-Build "清单的版本是 $($manifest.version) / $($manifest.build)，和 -Version $Version 对不上"
    }
}

# ---- 编译 ----
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$outFull = (Resolve-Path -LiteralPath $OutDir).Path
$baseName = "ModAVD-$Version-win-x64-setup"
if ($SelfTest) { $baseName += '-selftest' }

$isccArgs = @("/O$outFull", "/F$baseName", "/DAppVersion=$Version", "/DOutputBaseName=$baseName")
if ($SelfTest) { $isccArgs += '/DSelfTest' } else { $isccArgs += "/DPayloadDir=$payloadDir" }
$isccArgs += $issPath

Write-Host "ISCC $($isccArgs -join ' ')"
$watch = [Diagnostics.Stopwatch]::StartNew()
& $Iscc @isccArgs
$isccExit = $LASTEXITCODE
$watch.Stop()
if ($isccExit -ne 0) { Stop-Build "ISCC 失败，退出码 $isccExit" }

$exe = Join-Path $outFull "$baseName.exe"
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { Stop-Build "ISCC 说成功了，但没有 $exe" }
$item = Get-Item -LiteralPath $exe
$hash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host ('编好了：{0}' -f $exe)
Write-Host ('大小 {0:N0} 字节（{1:N1} MiB），用时 {2:N0} 秒' -f $item.Length, ($item.Length / 1MB), $watch.Elapsed.TotalSeconds)
Write-Host "SHA256 $hash"
[pscustomobject]@{ Path = $exe; Length = $item.Length; Sha256 = $hash; Seconds = [math]::Round($watch.Elapsed.TotalSeconds, 1) }
