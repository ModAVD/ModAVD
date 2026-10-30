# Copyright 2026 qiyinxi
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# This program is free software: you can redistribute it and/or modify it
# under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or (at your
# option) any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
# details. You should have received a copy of it along with this program; if
# not, see <https://www.gnu.org/licenses/>.

<#
魔改 AVD（ModAVD）：复制电脑配置和实例里装的游戏，方便试用时贴到群里统计。

双击 ModAVD 根目录的「复制配置信息.bat」即可。结果显示在窗口里，同时复制到剪贴板。

约束（改脚本时也要守住）：
- 必须能在 Windows PowerShell 5.1 上跑：不用 ??、三元、&&、-Parallel；文件存成 UTF-8 带 BOM。
- 只读：不开关实例、不起 adb server、不调用 adb.exe、不碰 5037。只在实例正开着、它的 adb server
  已经在监听时，按 adb 主机协议直接问装了哪些应用；没开就提示开着实例再运行一次。
- 不带用户名、电脑名、完整路径和游戏账号。

参数：
  -Root         ModAVD 根目录，默认脚本所在目录的上一级
  -NoClipboard  不复制到剪贴板（测试用）
#>
param(
    [string]$Root,
    [switch]$NoClipboard
)
$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }
$Root = [IO.Path]::GetFullPath($Root)

# 与后端 constants.py 一致：实例 i 的控制台端口 = PORT_BASE + PORT_STEP * i，默认 adb server 端口 20050
$PortBase = 20000
$PortStep = 10
$DefaultAdbPort = 20050

# 常见游戏包名 → 中文名；不认识的照原样显示包名
$GameNames = @{
    'com.hypergryph.arknights'          = '明日方舟（官服）'
    'com.hypergryph.arknights.bilibili' = '明日方舟（B 服）'
    'com.hypergryph.endfield'           = '明日方舟：终末地'
    'com.shenlan.m.reverse1999'         = '重返未来：1999'
    'com.miHoYo.hkrpg'                  = '崩坏：星穹铁道（官服）'
    'com.miHoYo.hkrpg.bilibili'         = '崩坏：星穹铁道（B 服）'
    'com.miHoYo.enterprise.NGHSoD'      = '崩坏3（官服）'
}
# 轻量桌面等随模拟器装的应用，不算游戏
$NotGames = @('de.jrpie.android.launcher')

$out = New-Object Collections.Generic.List[string]
function L([string]$Text) { $out.Add($Text) }
function Try-Get([scriptblock]$Body, $Fallback) { try { & $Body } catch { $Fallback } }

# ---------------------------------------------------------------- adb 主机协议（只连已在监听的 server）

function AdbSend($s, [string]$msg) {
    $b = [Text.Encoding]::UTF8.GetBytes($msg)
    $h = [Text.Encoding]::ASCII.GetBytes(('{0:x4}' -f $b.Length))
    $s.Write($h, 0, 4); $s.Write($b, 0, $b.Length)
}
function AdbReadExact($s, [int]$n) {
    $b = New-Object byte[] $n; $got = 0
    while ($got -lt $n) { $k = $s.Read($b, $got, $n - $got); if ($k -le 0) { throw 'adb server 断开' }; $got += $k }
    $b
}
function AdbStatus($s) {
    $st = [Text.Encoding]::ASCII.GetString((AdbReadExact $s 4))
    if ($st -eq 'OKAY') { return }
    if ($st -eq 'FAIL') {
        $len = [Convert]::ToInt32([Text.Encoding]::ASCII.GetString((AdbReadExact $s 4)), 16)
        throw ('adb server 返回 FAIL：' + [Text.Encoding]::UTF8.GetString((AdbReadExact $s $len)))
    }
    throw ('adb server 返回意外状态：' + $st)
}
function AdbOpen([int]$port) {
    $c = New-Object Net.Sockets.TcpClient
    $c.ReceiveTimeout = 15000; $c.SendTimeout = 5000
    $c.Connect('127.0.0.1', $port)
    $c
}
function AdbDevices([int]$port) {
    $c = AdbOpen $port
    try {
        $s = $c.GetStream(); AdbSend $s 'host:devices'; AdbStatus $s
        $len = [Convert]::ToInt32([Text.Encoding]::ASCII.GetString((AdbReadExact $s 4)), 16)
        if ($len -eq 0) { return '' }
        [Text.Encoding]::UTF8.GetString((AdbReadExact $s $len))
    } finally { $c.Close() }
}
function AdbShell([int]$port, [string]$serial, [string]$cmd) {
    $c = AdbOpen $port
    try {
        $s = $c.GetStream()
        AdbSend $s ('host:transport:' + $serial); AdbStatus $s
        AdbSend $s ('shell:' + $cmd); AdbStatus $s
        $ms = New-Object IO.MemoryStream; $buf = New-Object byte[] 8192
        while ($true) { $k = $s.Read($buf, 0, $buf.Length); if ($k -le 0) { break }; $ms.Write($buf, 0, $k) }
        [Text.Encoding]::UTF8.GetString($ms.ToArray()).Trim()
    } finally { $c.Close() }
}
function Test-Listening([int]$port) {
    try { return [bool](Get-NetTCPConnection -State Listen -LocalPort $port -ErrorAction Stop) } catch { }
    foreach ($line in @(netstat -ano -p TCP)) { if ($line -match ('^\s*TCP\s+\S+:' + $port + '\s+\S+\s+LISTENING')) { return $true } }
    return $false
}

# ---------------------------------------------------------------- 电脑

L '【魔改 AVD 试用配置】'

$os = Get-CimInstance Win32_OperatingSystem
$cv = Try-Get { Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' } $null
$ver = ''
if ($cv) {
    if ($cv.DisplayVersion) { $ver = [string]$cv.DisplayVersion } elseif ($cv.ReleaseId) { $ver = [string]$cv.ReleaseId }
    $ver = ('{0}（{1}.{2}）' -f $ver, $cv.CurrentBuild, $cv.UBR).Trim()
}
L ('系统：{0} {1}' -f ($os.Caption -replace '^Microsoft\s+', ''), $ver)

foreach ($cpu in @(Get-CimInstance Win32_Processor)) {
    L ('CPU：{0}（{1} 核 {2} 线程）' -f ($cpu.Name.Trim() -replace '\s+', ' '), $cpu.NumberOfCores, $cpu.NumberOfLogicalProcessors)
}
L ('内存：{0:N0} GB（现在可用 {1:N0} GB）' -f ([double]$os.TotalVisibleMemorySize / 1MB), ([double]$os.FreePhysicalMemory / 1MB))

# 只列实体显卡（PCI 设备），跳过远程桌面、串流软件装的虚拟显示适配器
$gpus = @(Get-CimInstance Win32_VideoController | Where-Object { [string]$_.PNPDeviceID -like 'PCI\*' })
if ($gpus.Count -eq 0) { $gpus = @(Get-CimInstance Win32_VideoController) }
foreach ($g in $gpus) {
    $dd = ''
    if ($g.DriverDate) { $dd = '，' + ([datetime]$g.DriverDate).ToString('yyyy-MM-dd') }
    L ('显卡：{0}（驱动 {1}{2}）' -f $g.Name, $g.DriverVersion, $dd)
}

# ---------------------------------------------------------------- ModAVD

$sdk = Join-Path $Root 'sdk'
$cfg = $null
$cfgPath = Join-Path $Root 'mas-avd.json'
if (Test-Path -LiteralPath $cfgPath) {
    $cfg = Try-Get { [IO.File]::ReadAllText($cfgPath, [Text.Encoding]::UTF8) | ConvertFrom-Json } $null
    if ($cfg -and $cfg.sdkRoot -is [string] -and $cfg.sdkRoot.Trim()) { $sdk = $cfg.sdkRoot.Trim() }
}
$build = '读不到'
$props = Join-Path $sdk 'emulator\source.properties'
if (Test-Path -LiteralPath $props) {
    foreach ($line in [IO.File]::ReadAllLines($props)) { if ($line -match '^Pkg\.BuildId=(.+)$') { $build = $Matches[1].Trim() } }
}
$diskText = ''
try {
    $letter = ([IO.Path]::GetPathRoot($Root)).Substring(0, 1)
    $drive = New-Object IO.DriveInfo ($letter + ':')
    $diskText = '，放在 {0}: 盘（剩余 {1:N0} GB' -f $letter, ($drive.AvailableFreeSpace / 1GB)
    $kind = Try-Get {
        $disk = Get-Partition -DriveLetter $letter -ErrorAction Stop | Get-Disk -ErrorAction Stop
        $pd = Get-PhysicalDisk -ErrorAction Stop | Where-Object { [string]$_.DeviceId -eq [string]$disk.Number } | Select-Object -First 1
        $mt = ''
        if ($pd -and [string]$pd.MediaType -and [string]$pd.MediaType -ne 'Unspecified') { $mt = [string]$pd.MediaType }
        ((@([string]$disk.BusType, $mt) | Where-Object { $_ }) -join ' ')
    } ''
    if ($kind) { $diskText += '，' + $kind }
    $diskText += '）'
} catch { }
L ('模拟器：{0}{1}' -f $build, $diskText)

# 硬件加速以模拟器自己的检查为准：按 Windows 功能名查不可靠（开着 Hyper-V 时，
# 「Windows 虚拟机监控程序平台」显示未开，WHPX 照样可用）
$accel = '读不到'
$ec = Join-Path $sdk 'emulator\emulator-check.exe'
if (Test-Path -LiteralPath $ec) {
    $tmp = [IO.Path]::GetTempFileName()
    try {
        $p = Start-Process -FilePath $ec -ArgumentList 'accel' -NoNewWindow -PassThru -RedirectStandardOutput $tmp -RedirectStandardError ($tmp + '.err')
        if (-not $p.WaitForExit(20000)) {
            try { $p.Kill() } catch { }
            $accel = '检查超时'
        } else {
            $msg = @(Get-Content -LiteralPath $tmp | ForEach-Object { $_.Trim() } | Where-Object { $_ -and $_ -notmatch '^(accel:?|\d+)$' }) -join ' '
            if ($msg -match '^(.+?) is installed and usable\.?$') { $accel = $Matches[1] + ' 可用' }
            elseif ($msg) { $accel = $msg }
        }
    } catch {
        $accel = '检查失败（' + $_.Exception.Message + '）'
    } finally {
        Remove-Item -LiteralPath $tmp, ($tmp + '.err') -Force -ErrorAction SilentlyContinue
    }
}
L ('硬件加速：' + $accel)

# 实例配置（mas-avd.json）
$instCfg = @{}
if ($cfg -and $cfg.instances) {
    foreach ($p in $cfg.instances.PSObject.Properties) { $instCfg[[string]$p.Name] = $p.Value }
}

# ---------------------------------------------------------------- 实例里装的游戏（只问开着的实例）

$ports = New-Object Collections.Generic.List[int]
$cfgPort = $null
if ($cfg -and $cfg.adbServerPort) { $cfgPort = Try-Get { [int]$cfg.adbServerPort } $null }
foreach ($p in @($cfgPort, $DefaultAdbPort, 20049)) {
    if ($p -and $p -ne 5037 -and -not $ports.Contains([int]$p)) { $ports.Add([int]$p) }
}
$seen = @{}
foreach ($port in $ports) {
    if (-not (Test-Listening $port)) { continue }
    $list = Try-Get { AdbDevices $port } ''
    foreach ($line in ($list -split "`n")) {
        $cport = $null; $serial = $null
        if ($line -match '^(emulator-(\d+))\s+device\s*$') { $serial = $Matches[1]; $cport = [int]$Matches[2] }
        elseif ($line -match '^(127\.0\.0\.1:(\d+))\s+device\s*$') { $serial = $Matches[1]; $cport = [int]$Matches[2] - 1 }
        if ($null -eq $cport) { continue }
        $off = $cport - $PortBase
        if ($off -lt 0 -or ($off % $PortStep) -ne 0) { continue }
        $idx = [string]($off / $PortStep)
        if ($seen.ContainsKey($idx)) { continue }
        $seen[$idx] = @{ Port = $port; Serial = $serial }
    }
}

$mem = { param($i) if ($instCfg.ContainsKey($i) -and $instCfg[$i].memoryMb) { '，{0:N0} GB' -f ([double]$instCfg[$i].memoryMb / 1024) } else { '' } }
if ($seen.Count -eq 0) {
    $names = @($instCfg.Keys | Sort-Object)
    if ($names.Count -gt 0) {
        L ('实例：{0} 个（{1}）' -f $names.Count, (($names | ForEach-Object { '实例 ' + $_ + (& $mem $_) }) -join '；'))
    }
    L '游戏包：没有实例开着，读不到。在 AUTO-MAS 里开着实例时再双击运行一次，就会列出来'
} else {
    foreach ($idx in ($seen.Keys | Sort-Object)) {
        $t = $seen[$idx]
        $games = @()
        try {
            if ((AdbShell $t.Port $t.Serial 'getprop sys.boot_completed') -ne '1') { throw '还没开完机' }
            $pkgs = @((AdbShell $t.Port $t.Serial 'pm list packages -3') -split "`n" | ForEach-Object { $_.Trim() } | Where-Object { $_ -like 'package:*' } | ForEach-Object { $_.Substring(8) } | Where-Object { $NotGames -notcontains $_ } | Sort-Object)
            foreach ($pkg in $pkgs) {
                # grep -m1 提前退出时 dumpsys 会往 stderr 补一行「Broken pipe」，shell 协议把它并进输出，只取 versionName 那一行
                $v = AdbShell $t.Port $t.Serial ('dumpsys package ' + $pkg + ' 2>/dev/null | grep -m1 versionName=')
                $v = @(($v -split "`n") | Where-Object { $_ -match '^\s*versionName=' } | ForEach-Object { ($_ -replace '^\s*versionName=', '').Trim() })[0]
                $name = $pkg
                if ($GameNames.ContainsKey($pkg)) { $name = $GameNames[$pkg] }
                if ($v) { $games += ('{0} {1}' -f $name, $v) } else { $games += $name }
            }
            if ($games.Count -eq 0) { $games = @('没装') }
        } catch {
            $games = @('读不到（' + $_.Exception.Message + '）')
        }
        L ('实例 {0}{1}：{2}' -f $idx, (& $mem $idx), ($games -join '、'))
    }
}

# ---------------------------------------------------------------- 让试用者手填的

L 'AUTO-MAS 版本：'
L '用的脚本（MAA / M9A / …）：'
L '效果（能跑 / 有问题，问题是…）：'

$text = $out -join "`r`n"
Write-Host ''
Write-Host $text
Write-Host ''
if (-not $NoClipboard) {
    try {
        Set-Clipboard -Value $text
        Write-Host '已复制到剪贴板：粘贴到群里，再把最后三行补上就行。'
    } catch {
        Write-Host ('复制到剪贴板失败（' + $_.Exception.Message + '），请手动选中上面的文字复制。')
    }
}
