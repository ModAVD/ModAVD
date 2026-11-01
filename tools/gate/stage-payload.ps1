#requires -Version 7.0
# Copyright 2026 qiyinxi
# SPDX-License-Identifier: AGPL-3.0-or-later

<#
把 arm64 执行桥的三个载荷文件放进一棵 SDK 树的 emulator\resources\modavd\。

这三个文件不是 AOSP 构建产物（两个是 bridge\ 的 NDK 产物，一个是 init.rc），所以 `tools\build\build-sdk.ps1`
编完之后要单独放进去。`tools\pubrel\release_tools.py` 的 assemble 对 `tools\pubrel\config\sdk-inventory.csv`
是**全等判定**：少一个文件就 `SDK inventory changed` 直接停，所以这一步不能漏，漏了也不会静默发出去。

载荷哈希固定在 `payload.SHA256SUMS.txt`（格式同 `tools\pubrel\config\components.SHA256SUMS.txt`：
`<小写 sha256><两个空格><resources/modavd/文件名>`，可以直接 `sha256sum -c`）。
重编过载荷之后必须用 `-UpdateSums` 重新固定一次，这样「换了一份二进制」在 git diff 里一定看得见。

    # 只核对载荷（不需要 SDK 树），编之前先跑这个
    pwsh -NoProfile -File tools\gate\stage-payload.ps1 -CheckOnly

    # 放进 SDK 树
    pwsh -NoProfile -File tools\gate\stage-payload.ps1 -Sdk <源码树>\sdk-mas36

    # 重编了载荷，重新固定哈希（会打印改了哪几行）
    pwsh -NoProfile -File tools\gate\stage-payload.ps1 -UpdateSums

参数：
  -Sdk          sdk-masN 根目录（里面有 emulator\）。-CheckOnly / -UpdateSums 时可以不给。
  -Payload      一个已冻结的目录，里面就是那三个文件。给了就只认这个目录，忽略下面三个单独路径。
                不给时取本机配置的 GatePayload（tools\local.example.psd1）。
  -Bridge / -Gate   两个 .so 各自的路径。不给时取本机配置的 BridgeSo / GateSo，再没有就是
                bridge\scripts\build-nbshim.ps1 / build-companion.ps1 不带 -OutDir 时编到的位置。
  -Rc           默认本仓库的 bridge\nbshim\modavd_bridge.rc；哈希固定，取错了分支也会停。
  -Sums         哈希固定文件，默认本目录的 payload.SHA256SUMS.txt。
  -CheckOnly    只核对源文件和哈希，不拷贝。
  -UpdateSums   用当前源文件的哈希重写固定文件，然后退出。
#>

[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$Sdk,
    [string]$Payload,
    [string]$Bridge,
    [string]$Gate,
    [string]$Rc = (Join-Path $PSScriptRoot '..\..\bridge\nbshim\modavd_bridge.rc'),
    [string]$Sums = (Join-Path $PSScriptRoot 'payload.SHA256SUMS.txt'),
    [switch]$CheckOnly,
    [switch]$UpdateSums
)

$ErrorActionPreference = 'Stop'
[cultureinfo]::CurrentCulture = [cultureinfo]::InvariantCulture
[cultureinfo]::CurrentUICulture = [cultureinfo]::InvariantCulture
. (Join-Path $PSScriptRoot '..\local.ps1')
if (-not $Payload -and -not $Bridge -and -not $Gate) { $Payload = Use-Local GatePayload $null '-Payload' -Optional }
if (-not $Bridge) { $Bridge = Use-Local BridgeSo $null '-Bridge' -Optional }
if (-not $Bridge) { $Bridge = Join-Path $env:TEMP 'modavd-nbbridge\nbshim\libs\x86_64\libmodavd_bridge.so' }
if (-not $Gate) { $Gate = Use-Local GateSo $null '-Gate' -Optional }
if (-not $Gate) { $Gate = Join-Path $env:TEMP 'modavd-nbbridge\companion\libs\x86_64\libmodavd_gate.so' }
$Rc = [IO.Path]::GetFullPath($Rc)

# 包里的位置。改这里要同时改 tools\pubrel\config\sdk-inventory.csv 和 package\modavd-bridge.ps1 的 -Payload 默认值。
$PackageDir = 'resources/modavd'
$Names = @('libmodavd_bridge.so', 'libmodavd_gate.so', 'modavd_bridge.rc')

function Sha([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }

# ---- 找源文件
$sources = [ordered]@{}
if ($Payload) {
    $dir = [IO.Path]::GetFullPath($Payload)
    foreach ($n in $Names) { $sources["$PackageDir/$n"] = (Join-Path $dir $n) }
} else {
    $sources["$PackageDir/libmodavd_bridge.so"] = $Bridge
    $sources["$PackageDir/libmodavd_gate.so"] = $Gate
    $sources["$PackageDir/modavd_bridge.rc"] = $Rc
}
$missing = @($sources.GetEnumerator() | Where-Object { -not (Test-Path -LiteralPath $_.Value -PathType Leaf) } | ForEach-Object { $_.Value })
if ($missing) { throw ("载荷源文件不存在：`n  " + ($missing -join "`n  ") + "`n（-Payload 指一个已冻结的目录，或者 -Bridge / -Gate / -Rc 分别指）") }

$actual = [ordered]@{}
foreach ($kv in $sources.GetEnumerator()) { $actual[$kv.Key] = Sha $kv.Value }
foreach ($kv in $sources.GetEnumerator()) {
    Write-Host ("{0,-40} {1}  {2,8} 字节  <- {3}" -f $kv.Key, $actual[$kv.Key], (Get-Item -LiteralPath $kv.Value).Length, $kv.Value)
}

# ---- 固定哈希
$lines = @()
foreach ($k in ($actual.Keys | Sort-Object)) { $lines += ("{0}  {1}" -f $actual[$k], $k) }
$text = ($lines -join "`n") + "`n"   # LF，和 tools\pubrel\config\components.SHA256SUMS.txt 一致

if ($UpdateSums) {
    $old = ''
    if (Test-Path -LiteralPath $Sums) { $old = [IO.File]::ReadAllText($Sums) }
    if ($old -eq $text) { Write-Host "固定哈希没有变化：$Sums"; return }
    if (-not $PSCmdlet.ShouldProcess($Sums, '重写载荷固定哈希')) { return }
    [IO.File]::WriteAllText($Sums, $text, [Text.UTF8Encoding]::new($false))
    Write-Host "已重写 $Sums"
    Write-Host '记得把这次改动和「载荷是从哪个提交的 bridge\ 编出来的」一起提交（写进提交说明）。'
    return
}

if (-not (Test-Path -LiteralPath $Sums)) { throw "找不到固定哈希文件：$Sums（第一次固定用 -UpdateSums）" }
$pinned = [ordered]@{}
foreach ($line in [IO.File]::ReadAllText($Sums, [Text.UTF8Encoding]::new($false)).Split("`n")) {
    $t = $line.Trim("`r", ' ')
    if (-not $t) { continue }
    $m = [regex]::Match($t, '^([0-9a-f]{64})  (.+)$')
    if (-not $m.Success) { throw "固定哈希文件里有读不懂的行：$t" }
    if ($pinned.Contains($m.Groups[2].Value)) { throw "固定哈希文件里有重复的路径：$($m.Groups[2].Value)" }
    $pinned[$m.Groups[2].Value] = $m.Groups[1].Value
}
$bad = @()
foreach ($k in $actual.Keys) {
    if (-not $pinned.Contains($k)) { $bad += "$k 没有被固定" }
    elseif ($pinned[$k] -ne $actual[$k]) { $bad += "$k 和固定值不同：固定 $($pinned[$k])，实际 $($actual[$k])" }
}
foreach ($k in $pinned.Keys) { if (-not $actual.Contains($k)) { $bad += "$k 被固定了，但这次没有这个文件" } }
if ($bad) {
    throw ("载荷和 $Sums 对不上：`n  " + ($bad -join "`n  ") +
        "`n重编过载荷就跑一次 `"stage-payload.ps1 -UpdateSums`" 重新固定，让这次换二进制在 git diff 里看得见。")
}
Write-Host "载荷与 $Sums 一致（$($actual.Count) 个文件）"

if ($CheckOnly) { return }

# ---- 放进 SDK 树
if (-not $Sdk) { throw '要 -Sdk（sdk-masN 根目录），或者用 -CheckOnly / -UpdateSums' }
$sdkRoot = [IO.Path]::GetFullPath($Sdk)
$emu = Join-Path $sdkRoot 'emulator'
if (-not (Test-Path -LiteralPath (Join-Path $emu 'emulator.exe'))) { throw "不像是 SDK 根目录（找不到 emulator\emulator.exe）：$sdkRoot" }
$dest = Join-Path $emu ($PackageDir -replace '/', '\')
if (-not $PSCmdlet.ShouldProcess($dest, "放 $($actual.Count) 个载荷文件")) { return }
if (-not (Test-Path -LiteralPath $dest)) { New-Item -ItemType Directory -Path $dest | Out-Null }
foreach ($kv in $sources.GetEnumerator()) {
    $name = Split-Path $kv.Key -Leaf
    $target = Join-Path $dest $name
    Copy-Item -LiteralPath $kv.Value -Destination $target -Force
    # 拷完再算一遍：源文件可能在中间被重编过
    $got = Sha $target
    if ($got -ne $actual[$kv.Key]) { throw "拷过去的 $name 哈希不对：$got（期望 $($actual[$kv.Key])）" }
    Write-Host "  + $target"
}
# 多出来的文件会让发版的全等判定直接停，所以这里就报出来
$extra = @(Get-ChildItem -LiteralPath $dest -File | Where-Object { $Names -notcontains $_.Name } | ForEach-Object { $_.Name })
if ($extra) { throw ("$dest 里有不该有的文件：" + ($extra -join '、') + "`n（发版的 SDK 文件集是全等判定，多一个就停）") }
Write-Host "载荷已就位：$dest"
