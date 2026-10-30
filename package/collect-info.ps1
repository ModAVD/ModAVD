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
魔改 AVD（ModAVD）：一键收集问题信息。

双击 ModAVD 根目录的「收集问题信息.bat」即可。脚本把排查需要的信息打成
「魔改AVD问题信息-<时间>.zip」放到桌面，然后打开资源管理器选中它。

约束（改脚本时也要守住）：
- 必须能在 Windows PowerShell 5.1 上跑：不用 ??、三元、&&、-Parallel；文件存成 UTF-8 带 BOM。
- 非管理员也要能跑完；每一节单独 try/catch，失败写进 summary 后继续。
- 只读：不关机、不重启、不起 adb server、不碰 5037；只删自己建的暂存子目录。
- 绝不拿磁盘镜像（*.img、*.qcow2、snapshots、userdata），绝不拿 MAS 的 config\ 和 data\。

参数：
  -Root      ModAVD 根目录（即 MAS 里配置的模拟器根目录），默认脚本所在目录的上一级
  -OutDir    zip 输出目录，默认桌面
  -StageDir  暂存目录的父目录，默认 $env:TEMP
  -KeepStage 不删暂存目录
#>
[CmdletBinding()]
param(
    [string]$Root,
    [string]$OutDir,
    [string]$StageDir,
    [switch]$KeepStage,
    # 测试用：假装读不到 qemu 命令行和 adb server 的进程路径（等同于非提权运行、AUTO-MAS 以管理员运行时）
    [switch]$SimulateNoCmdline
)

$ErrorActionPreference = 'Stop'
$script:StartTime = Get-Date
$script:Stamp = $script:StartTime.ToString('yyyyMMdd-HHmmss')

if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }
if (-not $OutDir) { $OutDir = [Environment]::GetFolderPath('Desktop') }
if (-not $StageDir) { $StageDir = $env:TEMP }
$Root = [IO.Path]::GetFullPath($Root)

$script:Summary = New-Object 'System.Collections.Generic.List[string]'
$script:Problems = New-Object 'System.Collections.Generic.List[string]'
$script:Timings = New-Object 'System.Collections.Generic.List[string]'
$script:Instances = @()
$script:SdkDir = $null
$script:AdbPort = 20050
$script:Running = @()
$script:CmdlineUnreadable = $false
$script:SdkSource = ''
$DefaultAdbPort = 20050
# 后端 constants.py 的端口常量：实例 i 的 console 端口 = PORT_BASE + i * PORT_STEP
$PortBase = 20000
$PortStep = 10
$script:MasFound = $false

$PrivacyNote = @(
    '隐私提示：问题包只保存在本机，脚本不会上传。'
    '- 里面有：summary.txt（电脑和模拟器的情况）、模拟器日志、实例配置、AUTO-MAS 日志（mas 文件夹）；有实例在运行时还有现场截图和安卓状态（live 文件夹）；模拟器近 7 天崩溃过的话还有崩溃转储（crash 文件夹）。不收磁盘镜像，不收 AUTO-MAS 的配置和账号数据，也不会有意收集账号密码。'
    '- 这些文件里都有本机路径，路径里可能有 Windows 用户名；截图和 AUTO-MAS 日志里可能有游戏账号名。'
    '- 可以公开：在 GitHub Issue（https://github.com/ModAVD/ModAVD/issues）里贴 summary.txt 和日志，或者上传删掉 crash 文件夹后的问题包。上传前看一眼，不想公开的内容先删掉。'
    '- 不要公开：crash 文件夹（崩溃转储，含模拟器进程的线程栈和少量内存片段）。带 crash 文件夹的完整问题包请发邮件到 qiyinxi@foxmail.com，邮件里写上 Issue 编号。'
) -join "`r`n"

$LogCapBytes = 300MB
$MasLogTailBytes = 30MB
$DumpMaxBytes = 100MB

# ---------------------------------------------------------------- 工具函数

function L([string]$Text = '') { $script:Summary.Add($Text) }

function Section([string]$Title) {
    L ''
    L ('== ' + $Title + ' ==')
}

function Invoke-Step([string]$Name, [scriptblock]$Body) {
    Write-Host ('  正在收集：' + $Name)
    $sw = [Diagnostics.Stopwatch]::StartNew()
    try {
        & $Body
    } catch {
        $msg = $_.Exception.Message
        L ('[本节出错] ' + $Name + '：' + $msg)
        $script:Problems.Add($Name + '：' + $msg)
    }
    $sw.Stop()
    $script:Timings.Add(('{0}：{1:N1} 秒' -f $Name, $sw.Elapsed.TotalSeconds))
}

function Format-Size([double]$Bytes) {
    if ($Bytes -ge 1GB) { return ('{0:N2} GB' -f ($Bytes / 1GB)) }
    if ($Bytes -ge 1MB) { return ('{0:N1} MB' -f ($Bytes / 1MB)) }
    if ($Bytes -ge 1KB) { return ('{0:N1} KB' -f ($Bytes / 1KB)) }
    return ('{0} B' -f [long]$Bytes)
}

$ShareAll = [IO.FileShare]'ReadWrite, Delete'

# 正在写的文件，目录列表里的大小会滞后（可能是 0），打开后取 Length 才准。
function Get-TrueLength([string]$Path) {
    $fs = $null
    try {
        $fs = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, $ShareAll)
        return [long]$fs.Length
    } catch {
        return [long](Get-Item -LiteralPath $Path -Force).Length
    } finally {
        if ($fs) { $fs.Dispose() }
    }
}

# 以共享读方式复制，正在被模拟器 / MAS 写的文件也能复制。TailBytes > 0 时只取文件末尾。
function Copy-Shared([string]$Source, [string]$Dest, [long]$TailBytes = 0) {
    $dir = Split-Path -Parent $Dest
    if (-not (Test-Path -LiteralPath $dir)) { [void](New-Item -ItemType Directory -Path $dir -Force) }
    $in = [IO.File]::Open($Source, [IO.FileMode]::Open, [IO.FileAccess]::Read, $ShareAll)
    try {
        if ($TailBytes -gt 0 -and $in.Length -gt $TailBytes) {
            [void]$in.Seek(-$TailBytes, [IO.SeekOrigin]::End)
        }
        $out = [IO.File]::Create($Dest)
        try { $in.CopyTo($out) } finally { $out.Dispose() }
    } finally {
        $in.Dispose()
    }
    return [long](Get-Item -LiteralPath $Dest).Length
}

function Read-Properties([string]$Path) {
    $h = @{}
    if (-not (Test-Path -LiteralPath $Path)) { return $h }
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        $i = $line.IndexOf('=')
        if ($i -gt 0 -and -not $line.TrimStart().StartsWith('#')) {
            $h[$line.Substring(0, $i).Trim()] = $line.Substring($i + 1).Trim()
        }
    }
    return $h
}

function Quote-Arg([string]$a) {
    if ($a -eq '') { return '""' }
    if ($a -notmatch '[\s"]') { return $a }
    return '"' + ($a -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}

# 起一个外部程序并限时；超时就结束它（只结束这个客户端进程本身）。
# StdoutFile 给出时，标准输出按二进制原样写进文件（screencap 用）。
function Invoke-Native {
    param(
        [string]$Exe,
        [string[]]$Arguments,
        [int]$TimeoutSec = 20,
        [string]$StdoutFile,
        [hashtable]$ExtraEnv
    )
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = (($Arguments | ForEach-Object { Quote-Arg $_ }) -join ' ')
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.WorkingDirectory = Split-Path -Parent $Exe
    if (-not $StdoutFile) { $psi.StandardOutputEncoding = [Text.Encoding]::UTF8 }
    $psi.StandardErrorEncoding = [Text.Encoding]::UTF8
    if ($ExtraEnv) { foreach ($k in $ExtraEnv.Keys) { $psi.EnvironmentVariables[$k] = [string]$ExtraEnv[$k] } }

    $p = [Diagnostics.Process]::Start($psi)
    $fs = $null
    $outTask = $null
    if ($StdoutFile) {
        $fs = [IO.File]::Create($StdoutFile)
        $outTask = $p.StandardOutput.BaseStream.CopyToAsync($fs)
    } else {
        $outTask = $p.StandardOutput.ReadToEndAsync()
    }
    $errTask = $p.StandardError.ReadToEndAsync()
    $timedOut = $false
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        $timedOut = $true
        try { $p.Kill() } catch { }
        [void]$p.WaitForExit(3000)
    }
    [void]$outTask.Wait(3000)
    [void]$errTask.Wait(3000)
    if ($fs) { $fs.Dispose() }
    $res = New-Object PSObject -Property @{
        TimedOut = $timedOut
        ExitCode = $(if ($timedOut) { $null } else { $p.ExitCode })
        Stdout   = $(if ($StdoutFile -or -not $outTask.IsCompleted) { '' } else { $outTask.Result })
        Stderr   = $(if ($errTask.IsCompleted) { $errTask.Result } else { '' })
    }
    $p.Dispose()
    return $res
}

# mas-avd.json 的 adbServerPort，照抄后端 components.py 的 _metadata_port：
# int(raw) 失败、不在 1024–65535、是 5037、或是任一实例（原生索引 0–9，5 保留）的控制台 / adb / gRPC 端口
# 时返回 $null（调用方回到 20050）。int() 对小数截断、对字符串要求是整数。
function Resolve-MetadataPort($Raw) {
    if ($null -eq $Raw) { return $null }
    $port = $null
    try {
        if ($Raw -is [bool]) { $port = [long]([int]$Raw) }
        elseif ($Raw -is [double] -or $Raw -is [single] -or $Raw -is [decimal]) { $port = [long][math]::Truncate([double]$Raw) }
        elseif ($Raw -is [int] -or $Raw -is [long] -or $Raw -is [int16] -or $Raw -is [byte]) { $port = [long]$Raw }
        elseif ($Raw -is [string]) {
            $s = $Raw.Trim()
            if ($s -match '^[+-]?\d+$') { $port = [long]$s }
        }
    } catch { $port = $null }
    if ($null -eq $port) { return $null }
    if ($port -lt 1024 -or $port -gt 65535 -or $port -eq 5037) { return $null }
    foreach ($i in 0..9) {
        if ($i -eq 5) { continue }
        $c = $PortBase + $PortStep * $i
        if ($port -ge $c -and $port -le $c + 2) { return $null }
    }
    return [int]$port
}

# 正在监听的 adb server：端口 -> pid / 路径。读不到就退回 netstat。
function Get-AdbListeners {
    $procs = @{}
    foreach ($wp in @(Get-CimInstance Win32_Process -Filter "Name='adb.exe'")) { $procs[[int]$wp.ProcessId] = $wp }
    $list = @()
    $seen = @{}
    $pairs = @()
    try {
        foreach ($c in @(Get-NetTCPConnection -State Listen -ErrorAction Stop)) {
            $pairs += New-Object PSObject -Property @{ Port = [int]$c.LocalPort; Pid = [int]$c.OwningProcess; Addr = [string]$c.LocalAddress }
        }
    } catch {
        foreach ($line in @(netstat -ano -p TCP)) {
            if ($line -match '^\s*TCP\s+(\S+):(\d+)\s+\S+\s+LISTENING\s+(\d+)') {
                $pairs += New-Object PSObject -Property @{ Port = [int]$Matches[2]; Pid = [int]$Matches[3]; Addr = $Matches[1] }
            }
        }
    }
    foreach ($pr in $pairs) {
        if (-not $procs.ContainsKey($pr.Pid)) { continue }
        $key = '{0}/{1}' -f $pr.Port, $pr.Pid
        if ($seen.ContainsKey($key)) { continue }
        $seen[$key] = $true
        $list += New-Object PSObject -Property @{
            Port = $pr.Port; Pid = $pr.Pid; Addr = $pr.Addr
            Path = $(if ($SimulateNoCmdline) { '' } else { [string]$procs[$pr.Pid].ExecutablePath })
        }
    }
    return @($list | Sort-Object Port)
}

# ---------------------------------------------------------------- 暂存目录

$StageDir = [IO.Path]::GetFullPath($StageDir)
if (-not (Test-Path -LiteralPath $StageDir)) { [void](New-Item -ItemType Directory -Path $StageDir -Force) }
$Stage = Join-Path $StageDir ('modavd-collect-' + $script:Stamp + '-' + $PID)
[void](New-Item -ItemType Directory -Path $Stage)
# 规范路径（去掉 ..）；打包时条目名另按相对路径逐级拼，不靠截取 FullName
$Stage = [IO.Path]::GetFullPath((Get-Item -LiteralPath $Stage).FullName)

Write-Host ''
Write-Host '魔改 AVD 问题信息收集'
Write-Host ('ModAVD 目录：' + $Root)
Write-Host ''

L '魔改 AVD 问题信息'
L $PrivacyNote
L ('生成时间：' + $script:StartTime.ToString('yyyy-MM-dd HH:mm:ss zzz'))
L ('PowerShell：' + $PSVersionTable.PSVersion.ToString() + '（' + $PSVersionTable.PSEdition + '）')

# ---------------------------------------------------------------- 1. ModAVD 目录与版本

Invoke-Step 'ModAVD 目录与版本' {
    Section 'ModAVD 目录'
    L ('路径：' + $Root)
    if ($Root -match '[^\x00-\x7F]') {
        L '  警告：路径里有中文或其他非 ASCII 字符，模拟器可能起不来，请把 ModAVD 放到纯英文路径下（例如 D:\ModAVD）。'
    }
    if (-not (Test-Path -LiteralPath $Root)) { L '  警告：这个目录不存在。' }
    try {
        $drive = New-Object IO.DriveInfo ([IO.Path]::GetPathRoot($Root))
        L ('所在盘：{0} 剩余 {1} / 共 {2}（{3}）' -f $drive.Name, (Format-Size $drive.AvailableFreeSpace), (Format-Size $drive.TotalSize), $drive.DriveFormat)
    } catch {
        L ('所在盘剩余空间：读不到（' + $_.Exception.Message + '）')
    }

    $cfgPath = Join-Path $Root 'mas-avd.json'
    # 和后端 read_metadata 一致：不存在、读不出、不是 JSON 对象，都按空配置处理
    $cfg = $null
    if (Test-Path -LiteralPath $cfgPath) {
        try { $cfg = [IO.File]::ReadAllText($cfgPath, [Text.Encoding]::UTF8) | ConvertFrom-Json } catch { L ('mas-avd.json 解析失败，按空配置处理：' + $_.Exception.Message) }
        if ($null -ne $cfg -and $cfg -isnot [Management.Automation.PSCustomObject]) { L 'mas-avd.json 不是 JSON 对象，按空配置处理'; $cfg = $null }
    }

    # adb server 端口：规则同后端 adb_server_port()，不合规回到 20050；5037 一律不用
    $rawPort = $null
    $hasPort = $false
    if ($cfg -and ($cfg.PSObject.Properties.Name -contains 'adbServerPort')) { $hasPort = $true; $rawPort = $cfg.adbServerPort }
    $okPort = Resolve-MetadataPort $rawPort
    if ($null -ne $okPort) {
        $script:AdbPort = $okPort
        L ('adb server 端口：{0}（mas-avd.json 的 adbServerPort）' -f $okPort)
    } else {
        $script:AdbPort = $DefaultAdbPort
        if ($hasPort) {
            L ('adb server 端口：{0}（mas-avd.json 的 adbServerPort = {1} 不合规：要 1024–65535 的整数、不是 5037、不占实例端口；按后端规则回到 {0}）' -f $DefaultAdbPort, ($rawPort | ConvertTo-Json -Compress))
        } else {
            L ('adb server 端口：{0}（mas-avd.json 没有配置，按默认）' -f $DefaultAdbPort)
        }
    }

    # SDK：和后端 runtime_sdk_dir() 一致，mas-avd.json 的 sdkRoot（非空字符串）优先，没有才用 <根>\sdk
    $sdkRoot = $null
    if ($cfg -and ($cfg.sdkRoot -is [string]) -and $cfg.sdkRoot.Trim()) { $sdkRoot = $cfg.sdkRoot.Trim() }
    if ($sdkRoot) {
        $script:SdkDir = $sdkRoot
        $script:SdkSource = 'mas-avd.json 的 sdkRoot'
        L ('SDK：{0}（mas-avd.json 的 sdkRoot；后端优先用它，包内 sdk 不用）' -f $sdkRoot)
        if (-not (Test-Path -LiteralPath (Join-Path $sdkRoot 'emulator'))) { L '  警告：sdkRoot 指向的目录里没有 emulator，后端照样会用它，模拟器起不来。' }
    } else {
        $script:SdkDir = Join-Path $Root 'sdk'
        $script:SdkSource = '包内 sdk'
        L ('SDK：{0}（包内 sdk）' -f $script:SdkDir)
        if (-not (Test-Path -LiteralPath (Join-Path $script:SdkDir 'emulator'))) { L '  警告：找不到 sdk\emulator，模拟器文件不全。' }
    }

    Section '模拟器版本'
    if ($script:SdkDir) {
        $emu = Read-Properties (Join-Path $script:SdkDir 'emulator\source.properties')
        L ('emulator：Pkg.Revision={0}  Pkg.BuildId={1}' -f $emu['Pkg.Revision'], $emu['Pkg.BuildId'])
        $pt = Read-Properties (Join-Path $script:SdkDir 'platform-tools\source.properties')
        L ('platform-tools：Pkg.Revision={0}' -f $pt['Pkg.Revision'])
        $imgs = @(Get-ChildItem -LiteralPath (Join-Path $script:SdkDir 'system-images') -Recurse -Filter 'source.properties' -ErrorAction SilentlyContinue)
        if ($imgs.Count -eq 0) { L '系统镜像：没找到 system-images\…\source.properties' }
        foreach ($f in $imgs) {
            $sp = Read-Properties $f.FullName
            $rel = $f.DirectoryName.Substring($script:SdkDir.Length).TrimStart('\')
            L ('系统镜像 {0}：Pkg.Revision={1} ApiLevel={2} Abi={3} TagId={4} Desc={5}' -f $rel, $sp['Pkg.Revision'], $sp['AndroidVersion.ApiLevel'], $sp['SystemImage.Abi'], $sp['SystemImage.TagId'], $sp['Pkg.Desc'])
        }
    }
}

# ---------------------------------------------------------------- 2. 本机

Invoke-Step 'Windows / CPU / 内存 / 显卡' {
    Section '本机'
    $os = Get-CimInstance Win32_OperatingSystem
    $ubr = $null
    try { $ubr = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' -Name UBR).UBR } catch { }
    L ('Windows：{0}  Version={1}  BuildNumber={2}  UBR={3}  {4}' -f $os.Caption, $os.Version, $os.BuildNumber, $ubr, $os.OSArchitecture)
    foreach ($cpu in @(Get-CimInstance Win32_Processor)) {
        L ('CPU：{0}  物理核 {1}  逻辑核 {2}  VirtualizationFirmwareEnabled={3}' -f $cpu.Name.Trim(), $cpu.NumberOfCores, $cpu.NumberOfLogicalProcessors, $cpu.VirtualizationFirmwareEnabled)
    }
    try {
        $cs = Get-CimInstance Win32_ComputerSystem
        L ('HypervisorPresent={0}（为 True 时 VirtualizationFirmwareEnabled 显示 False 是正常的）' -f $cs.HypervisorPresent)
    } catch { }
    L ('内存：共 {0}，可用 {1}' -f (Format-Size ([double]$os.TotalVisibleMemorySize * 1KB)), (Format-Size ([double]$os.FreePhysicalMemory * 1KB)))
    foreach ($g in @(Get-CimInstance Win32_VideoController)) {
        $dd = ''
        if ($g.DriverDate) { $dd = ([datetime]$g.DriverDate).ToString('yyyy-MM-dd') }
        L ('显卡：{0}  DriverVersion={1}  DriverDate={2}' -f $g.Name, $g.DriverVersion, $dd)
    }
}

Invoke-Step '虚拟化功能' {
    # 开了 Hyper-V 时 HypervisorPlatform 显示未开、WHPX 照样可用（作者机器 26200 实测），能不能用以下一节 accel-check 为准
    Section '虚拟化相关 Windows 功能（InstallState 1=已开 2=未开 3=缺失；仅供参考，能不能用看下一节 accel-check）'
    $names = @('HypervisorPlatform', 'VirtualMachinePlatform', 'Microsoft-Hyper-V-All')
    $feat = @{}
    foreach ($f in @(Get-CimInstance Win32_OptionalFeature -Filter "Name='HypervisorPlatform' or Name='VirtualMachinePlatform' or Name='Microsoft-Hyper-V-All'")) {
        $feat[$f.Name] = $f.InstallState
    }
    foreach ($n in $names) {
        if ($feat.ContainsKey($n)) {
            $st = $feat[$n]
            $txt = '未知'
            if ($st -eq 1) { $txt = '已开' } elseif ($st -eq 2) { $txt = '未开' } elseif ($st -eq 3) { $txt = '缺失' }
            L ('{0}：InstallState={1}（{2}）' -f $n, $st, $txt)
        } else {
            L ($n + '：没查到')
        }
    }
}

Invoke-Step '加速检查（emulator -accel-check）' {
    Section 'emulator -accel-check'
    if (-not $script:SdkDir) { L '跳过：没有 sdk\emulator'; return }
    $exe = Join-Path $script:SdkDir 'emulator\emulator.exe'
    if (-not (Test-Path -LiteralPath $exe)) { L ('跳过：没有 ' + $exe); return }
    $r = Invoke-Native -Exe $exe -Arguments @('-accel-check') -TimeoutSec 30
    if ($r.TimedOut) { L '超时（30 秒），已放弃' }
    L ('退出码：' + $r.ExitCode)
    foreach ($line in (($r.Stdout + "`n" + $r.Stderr) -split "`r?`n")) { if ($line.Trim()) { L ('  ' + $line.TrimEnd()) } }
}

Invoke-Step '杀毒软件' {
    Section '杀毒软件（SecurityCenter2）'
    $av = @(Get-CimInstance -Namespace 'root/SecurityCenter2' -ClassName AntiVirusProduct)
    if ($av.Count -eq 0) { L '没查到' }
    foreach ($a in $av) { L ('{0}  productState=0x{1:X6}' -f $a.displayName, [int]$a.productState) }
}

# ---------------------------------------------------------------- 3. 实例

Invoke-Step '实例列表与实例文件' {
    Section '实例'
    $avdDir = Join-Path $Root 'avd'
    $inis = @()
    if (Test-Path -LiteralPath $avdDir) { $inis = @(Get-ChildItem -LiteralPath $avdDir -Filter '*.ini' -File) }
    if ($inis.Count -eq 0) {
        L '还没有实例（avd 目录不存在或里面没有 *.ini）。'
    }
    # 实例名先全部登记（只要 ini 在），后面哪一台出错都不影响现场采集认它
    $script:Instances = @($inis | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_.Name) })
    foreach ($ini in $inis) {
        $name = [IO.Path]::GetFileNameWithoutExtension($ini.Name)
        # 每台单独 try：一台残缺或出错只记这一台，继续下一台
        try {
            [void](Copy-Shared $ini.FullName (Join-Path $Stage ('avd\' + $ini.Name)))
            $dir = Join-Path $avdDir ($name + '.avd')
            if (-not (Test-Path -LiteralPath $dir)) {
                L ($name + '：残缺——有 ' + $ini.Name + '，但缺少 ' + $name + '.avd 目录')
                continue
            }
            $outDir = Join-Path $Stage ('avd\' + $name + '.avd')
            if (-not (Test-Path -LiteralPath $outDir)) { [void](New-Item -ItemType Directory -Path $outDir -Force) }
            $present = @(@('config.ini', 'hardware-qemu.ini', 'emulator-user.ini') | Where-Object { Test-Path -LiteralPath (Join-Path $dir $_) -PathType Leaf })
            if ($present.Count -eq 0) {
                L ('{0}：残缺——{0}.avd 里 config.ini、hardware-qemu.ini、emulator-user.ini 一个都没有（多半是删实例删到一半）' -f $name)
            } else {
                $cfg = Read-Properties (Join-Path $dir 'config.ini')
                L ('{0}：hw.ramSize={1}  hw.cpu.ncore={2}  hw.lcd={3}x{4} density={5}  image.sysdir.1={6}' -f $name, $cfg['hw.ramSize'], $cfg['hw.cpu.ncore'], $cfg['hw.lcd.width'], $cfg['hw.lcd.height'], $cfg['hw.lcd.density'], $cfg['image.sysdir.1'])
                if ($present -notcontains 'config.ini') { L ('  警告：{0}.avd 里没有 config.ini' -f $name) }
            }
            foreach ($fn in $present) {
                try { [void](Copy-Shared (Join-Path $dir $fn) (Join-Path $outDir $fn)) }
                catch { L ('  复制 ' + $fn + ' 失败：' + $_.Exception.Message) }
            }
            # 文件名清单：只要名字和大小，不拿内容
            $lst = New-Object 'System.Collections.Generic.List[string]'
            foreach ($it in @(Get-ChildItem -LiteralPath $dir -Force)) {
                if ($it.PSIsContainer) { $lst.Add(('{0,-40} <DIR>' -f $it.Name)) }
                else { $lst.Add(('{0,-40} {1,16:N0}  {2}' -f $it.Name, $it.Length, $it.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))) }
            }
            if ($lst.Count -eq 0) { $lst.Add('（空目录）') }
            [IO.File]::WriteAllLines((Join-Path $outDir '_files.txt'), $lst, (New-Object Text.UTF8Encoding $true))
        } catch {
            L ('{0}：收集这台实例时出错：{1}' -f $name, $_.Exception.Message)
            $script:Problems.Add('实例 ' + $name + '：' + $_.Exception.Message)
        }
    }
    $cfgPath = Join-Path $Root 'mas-avd.json'
    if (Test-Path -LiteralPath $cfgPath) {
        try { [void](Copy-Shared $cfgPath (Join-Path $Stage 'mas-avd.json')) }
        catch { L ('复制 mas-avd.json 失败：' + $_.Exception.Message) }
    } else {
        L '没有 mas-avd.json'
    }
}

Invoke-Step '正在运行的模拟器与 adb' {
    Section '正在运行的 qemu-system 进程'
    $qs = @(Get-CimInstance Win32_Process -Filter "Name like 'qemu-system%'")
    if ($qs.Count -eq 0) { L '没有' }
    $running = @()
    foreach ($q in $qs) {
        $ct = ''
        if ($q.CreationDate) { $ct = ([datetime]$q.CreationDate).ToString('yyyy-MM-dd HH:mm:ss') }
        L ('pid {0}  启动于 {1}' -f $q.ProcessId, $ct)
        $cmd = [string]$q.CommandLine
        if ($SimulateNoCmdline) { $cmd = '' }
        if (-not $cmd) {
            L '  qemu 命令行读不到（多半是 AUTO-MAS 以管理员运行），现场按 adb 设备列表推算'
            $script:CmdlineUnreadable = $true
            continue
        }
        L ('  ' + $cmd)
        $avd = $null
        if ($cmd -match '(?:^|\s)-avd\s+"?([^\s"]+)') { $avd = $Matches[1] }
        $port = $null
        if ($cmd -match '(?:^|\s)-ports?\s+"?(\d+)') { $port = [int]$Matches[1] }
        if ($avd -and ($script:Instances -contains $avd)) {
            $running += New-Object PSObject -Property @{ Name = $avd; Pid = [int]$q.ProcessId; ConsolePort = $port; Inferred = $false }
        }
    }
    $script:Running = $running

    Section '正在监听的 adb server'
    $ls = @(Get-AdbListeners)
    if ($ls.Count -eq 0) { L '没有' }
    foreach ($a in $ls) {
        $p = $a.Path
        if (-not $p) { $p = '（路径读不到）' }
        L ('端口 {0}（{1}）  pid {2}  {3}' -f $a.Port, $a.Addr, $a.Pid, $p)
    }
}

# ---------------------------------------------------------------- 4. 日志

Invoke-Step '模拟器日志' {
    Section '模拟器日志（logs\）'
    $logDir = Join-Path $Root 'logs'
    if (-not (Test-Path -LiteralPath $logDir)) { L '没有 logs 目录'; return }
    $all = @(Get-ChildItem -LiteralPath $logDir -File)
    $names = @{}
    foreach ($n in $script:Instances) { $names[$n] = $true }
    foreach ($f in $all) {
        if ($f.Name -match '^(?:logcat-)?(.+)-\d{8}-\d{6}\.(?:log|txt)$') { $names[$Matches[1]] = $true }
    }
    $cand = @()
    foreach ($n in @($names.Keys | Sort-Object)) {
        $esc = [regex]::Escape($n)
        # 文件名里的时间戳就是开机时间，按名字倒序即新到旧
        $emuLogs = @($all | Where-Object { $_.Name -match ('^' + $esc + '-\d{8}-\d{6}\.log$') } | Sort-Object Name -Descending | Select-Object -First 8)
        $catLogs = @($all | Where-Object { $_.Name -match ('^logcat-' + $esc + '-\d{8}-\d{6}\.txt$') } | Sort-Object Name -Descending | Select-Object -First 8)
        foreach ($f in $emuLogs) { $cand += New-Object PSObject -Property @{ File = $f; Kind = 'emu'; Stamp = ($f.Name -replace '^.*-(\d{8}-\d{6})\..*$', '$1'); Len = (Get-TrueLength $f.FullName) } }
        foreach ($f in $catLogs) { $cand += New-Object PSObject -Property @{ File = $f; Kind = 'logcat'; Stamp = ($f.Name -replace '^.*-(\d{8}-\d{6})\..*$', '$1'); Len = (Get-TrueLength $f.FullName) } }
        L ('{0}：模拟器日志 {1} 份，logcat {2} 份（各取最新 8 份）' -f $n, $emuLogs.Count, $catLogs.Count)
    }
    $total = [long]0
    foreach ($c in $cand) { $total += $c.Len }
    $dropped = 0
    if ($total -gt $LogCapBytes) {
        # 超过上限：先丢最旧的 logcat，再丢最旧的模拟器日志
        $order = @($cand | Sort-Object @{ Expression = { if ($_.Kind -eq 'logcat') { 0 } else { 1 } } }, Stamp)
        $keep = @{}
        foreach ($c in $cand) { $keep[$c.File.FullName] = $true }
        foreach ($c in $order) {
            if ($total -le $LogCapBytes) { break }
            $keep[$c.File.FullName] = $false
            $total -= $c.Len
            $dropped++
        }
        $cand = @($cand | Where-Object { $keep[$_.File.FullName] })
        L ('原始总量超过 {0}，丢掉了最旧的 {1} 份' -f (Format-Size $LogCapBytes), $dropped)
    }
    $copied = [long]0
    $n = 0
    foreach ($c in $cand) {
        try {
            $copied += Copy-Shared $c.File.FullName (Join-Path $Stage ('logs\' + $c.File.Name))
            $n++
        } catch {
            L ('  复制 ' + $c.File.Name + ' 失败：' + $_.Exception.Message)
        }
    }
    L ('共复制 {0} 份，{1}' -f $n, (Format-Size $copied))

    # 应用启动卡住时的线程转储，各十几 KB，取最新 3 份（按文件名里的时间戳，不计入上面的上限）
    $hang = @($all | Where-Object { $_.Name -like 'startup-*-hang-threads.txt' } |
        Sort-Object @{ Expression = { if ($_.Name -match '-(\d{8}-\d{6})-') { $Matches[1] } else { $_.LastWriteTime.ToString('yyyyMMdd-HHmmss') } } }, Name -Descending |
        Select-Object -First 3)
    if ($hang.Count -eq 0) {
        L '启动卡死线程转储（startup-*-hang-threads.txt）：没有'
    } else {
        $hn = 0
        foreach ($f in $hang) {
            try { [void](Copy-Shared $f.FullName (Join-Path $Stage ('logs\' + $f.Name))); $hn++ }
            catch { L ('  复制 ' + $f.Name + ' 失败：' + $_.Exception.Message) }
        }
        L ('启动卡死线程转储（startup-*-hang-threads.txt）：取了最新 {0} 份：{1}' -f $hn, (($hang | ForEach-Object { $_.Name }) -join '、'))
    }

    # 补全组件脚本（fetch-components.ps1）的日志：下载 adb / 系统镜像失败时靠它看是哪个镜像、哪一步
    $fc = Join-Path $logDir 'fetch-components.log'
    if (Test-Path -LiteralPath $fc) {
        try { [void](Copy-Shared $fc (Join-Path $Stage 'logs\fetch-components.log')); L '补全组件日志（fetch-components.log）：已收' }
        catch { L ('补全组件日志（fetch-components.log）：复制失败：' + $_.Exception.Message) }
    } else {
        L '补全组件日志（fetch-components.log）：没有'
    }
}

Invoke-Step '模拟器崩溃转储' {
    Section '模拟器崩溃转储'
    $bases = @()
    foreach ($b in @((Join-Path $env:LOCALAPPDATA 'Temp\AndroidEmulator'), (Join-Path $env:TEMP 'AndroidEmulator'))) {
        if ($b -and (Test-Path -LiteralPath $b)) {
            $full = [IO.Path]::GetFullPath($b)
            if ($bases -notcontains $full) { $bases += $full }
        }
    }
    $since = (Get-Date).AddDays(-7)
    $dumps = @()
    foreach ($b in $bases) {
        foreach ($db in @(Get-ChildItem -LiteralPath $b -Directory -Filter 'emu-crash-*.db' -ErrorAction SilentlyContinue)) {
            $dumps += @(Get-ChildItem -LiteralPath $db.FullName -Recurse -File -Filter '*.dmp' -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -ge $since })
        }
    }
    $dumps = @($dumps | Sort-Object LastWriteTime -Descending | Select-Object -First 3)
    if ($dumps.Count -eq 0) { L '近 7 天没有'; return }
    foreach ($d in $dumps) {
        $info = '{0}  {1}  {2}' -f $d.FullName, $d.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'), (Format-Size $d.Length)
        if ($d.Length -gt $DumpMaxBytes) { L ($info + '（超过 ' + (Format-Size $DumpMaxBytes) + '，没拿）'); continue }
        $dbName = Split-Path -Leaf (($d.FullName -replace '(\\emu-crash-[^\\]+\.db)\\.*$', '$1'))
        [void](Copy-Shared $d.FullName (Join-Path $Stage ('crash\' + $dbName + '\' + $d.Name)))
        L $info
    }
}

Invoke-Step 'AUTO-MAS 日志' {
    Section 'AUTO-MAS'
    $loc = $null
    $ver = $null
    $keys = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*'
    )
    foreach ($k in $keys) {
        foreach ($e in @(Get-ItemProperty $k -ErrorAction SilentlyContinue)) {
            if ([string]$e.DisplayName -match '^AUTO-MAS' -and $e.InstallLocation -and (Test-Path -LiteralPath ([string]$e.InstallLocation))) {
                $loc = [string]$e.InstallLocation
                $ver = [string]$e.DisplayVersion
                break
            }
        }
        if ($loc) { break }
    }
    $how = '卸载注册表'
    if (-not $loc) {
        foreach ($wp in @(Get-CimInstance Win32_Process -Filter "Name='AUTO-MAS.exe'")) {
            if ($wp.ExecutablePath) { $loc = Split-Path -Parent $wp.ExecutablePath; $how = '正在运行的 AUTO-MAS.exe'; break }
        }
    }
    if (-not $loc) {
        L '没找到 AUTO-MAS 安装目录，反馈时请附上 AUTO-MAS 的 debug 文件夹里的日志'
        return
    }
    $script:MasFound = $true
    $exe = Join-Path $loc 'AUTO-MAS.exe'
    if (-not $ver -and (Test-Path -LiteralPath $exe)) { $ver = (Get-Item -LiteralPath $exe).VersionInfo.ProductVersion }
    L ('安装目录：{0}（来自{1}）' -f $loc, $how)
    L ('版本：' + $ver)
    $dbg = Join-Path $loc 'debug'
    if (-not (Test-Path -LiteralPath $dbg)) { L '安装目录下没有 debug 文件夹'; return }
    # 只拿 debug\ 里的日志；config\、data\ 里有加密过的账号密码，绝不拿
    foreach ($fn in @('app.log', 'frontend.log')) {
        $src = Join-Path $dbg $fn
        if (-not (Test-Path -LiteralPath $src)) { L ($fn + '：没有'); continue }
        try {
            $len = Get-TrueLength $src
            $got = Copy-Shared $src (Join-Path $Stage ('mas\' + $fn)) $MasLogTailBytes
            if ($len -gt $MasLogTailBytes) { L ('{0}：{1}，只取了最后 {2}' -f $fn, (Format-Size $len), (Format-Size $got)) }
            else { L ('{0}：{1}' -f $fn, (Format-Size $got)) }
        } catch {
            L ('复制 ' + $fn + ' 失败：' + $_.Exception.Message)
        }
    }
    $zip = @(Get-ChildItem -LiteralPath $dbg -File -Filter 'app.*.log.zip' | Sort-Object LastWriteTime -Descending | Select-Object -First 1)
    if ($zip.Count -gt 0) {
        $got = Copy-Shared $zip[0].FullName (Join-Path $Stage ('mas\' + $zip[0].Name))
        L ('{0}：{1}' -f $zip[0].Name, (Format-Size $got))
    } else {
        L '没有轮转压缩包 app.*.log.zip'
    }
}

Invoke-Step '系统事件日志' {
    Section '近 7 天 Application 日志（ID 1000/1001/1002，含 qemu / emulator / adb / AUTO-MAS）'
    $ev = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000, 1001, 1002; StartTime = (Get-Date).AddDays(-7) } -MaxEvents 2000 -ErrorAction SilentlyContinue)
    # adb 要成词：WER 报告路径里的十六进制哈希常含 "adb"
    $hit = @($ev | Where-Object { $_.Message -match 'qemu|emulator|\badb\b|AUTO-MAS' } | Select-Object -First 30)
    if ($hit.Count -eq 0) { L '没有'; return }
    foreach ($e in $hit) {
        L ('{0}  ID {1}  {2}' -f $e.TimeCreated.ToString('yyyy-MM-dd HH:mm:ss'), $e.Id, $e.ProviderName)
        $lines = @(([string]$e.Message -split "`r?`n") | Where-Object { $_.Trim() } | Select-Object -First 6)
        foreach ($ln in $lines) { L ('    ' + $ln.Trim()) }
    }
}

# ---------------------------------------------------------------- 5. 现场（只在有实例正在运行时）

Invoke-Step '现场截图与状态' {
    Section '现场'
    if ($script:Running.Count -eq 0 -and -not $script:CmdlineUnreadable) { L '没有实例在运行，跳过'; return }
    # 只用已经在监听的 adb server；绝不自己起 server，绝不用 5037
    if ($script:AdbPort -eq 5037) { L 'adb 端口配置成了 5037，按规定不用，跳过'; return }
    $listener = @(Get-AdbListeners | Where-Object { $_.Port -eq $script:AdbPort } | Select-Object -First 1)
    if ($listener.Count -eq 0) { L ('端口 {0} 上没有 adb server 在监听，跳过（不自己起 server）' -f $script:AdbPort); return }
    # 优先用 server 自己那个 adb.exe，避免客户端版本不同把 server 重启掉
    $adb = $listener[0].Path
    $adbFrom = 'adb server 自己的 adb.exe'
    if (-not $adb -or -not (Test-Path -LiteralPath $adb)) {
        # server 是提权进程时读不到路径；后端起 server 用的就是这个 SDK 的 platform-tools，版本一致
        $adb = $null
        if ($script:SdkDir) { $adb = Join-Path $script:SdkDir 'platform-tools\adb.exe' }
        $adbFrom = 'server 进程路径读不到，用 SDK（' + $script:SdkSource + '）的 platform-tools'
    }
    if (-not $adb -or -not (Test-Path -LiteralPath $adb)) { L '找不到 adb.exe，跳过'; return }
    L ('adb：{0}  -P {1}（{2}）' -f $adb, $script:AdbPort, $adbFrom)
    $env:ADB_LOCAL_TRANSPORT_MAX_PORT = '5554'
    $adbEnv = @{ ADB_LOCAL_TRANSPORT_MAX_PORT = '5554' }
    $liveDir = Join-Path $Stage 'live'
    [void](New-Item -ItemType Directory -Path $liveDir -Force)
    $enc = New-Object Text.UTF8Encoding $false

    $targets = @($script:Running)
    if ($script:CmdlineUnreadable) {
        # 退路：qemu 命令行读不到时，用 adb 设备列表的 emulator-<端口> 按后端端口常量推算实例名
        L 'qemu 命令行读不到（多半是 AUTO-MAS 以管理员运行），现场按 adb 设备列表推算'
        $res = Invoke-Native -Exe $adb -Arguments @('-P', [string]$script:AdbPort, 'devices') -TimeoutSec 20 -ExtraEnv $adbEnv
        if ($res.TimedOut) {
            L '  adb devices：超时（20 秒），已放弃'
        } else {
            $found = 0
            foreach ($line in @($res.Stdout -split "`r?`n")) {
                if ($line -notmatch '^emulator-(\d+)\s+(\S+)') { continue }
                $found++
                $cport = [int]$Matches[1]
                $state = $Matches[2]
                if (@($targets | Where-Object { $_.ConsolePort -eq $cport }).Count -gt 0) { continue }
                $off = $cport - $PortBase
                if ($off -lt 0 -or ($off % $PortStep) -ne 0) {
                    L ('  emulator-{0}：端口不符合 PORT_BASE={1}、PORT_STEP={2}，推算不出实例，跳过' -f $cport, $PortBase, $PortStep)
                    continue
                }
                $name = 'mas_' + [int]($off / $PortStep)
                L ('  emulator-{0}（{1}）→ {2}：按 ({0} - {3}) / {4} 推算（PORT_BASE、PORT_STEP 是后端 constants.py 的常量）' -f $cport, $state, $name, $PortBase, $PortStep)
                if ($state -ne 'device') { L '    状态不是 device，跳过'; continue }
                if ($script:Instances -notcontains $name) { L '    本 ModAVD 目录的 avd 下没有这个实例，跳过'; continue }
                $targets += New-Object PSObject -Property @{ Name = $name; Pid = $null; ConsolePort = $cport; Inferred = $true }
            }
            if ($found -eq 0) { L ('  adb devices 里没有 emulator-* 设备（退出码 {0}）' -f $res.ExitCode) }
        }
    }
    if ($targets.Count -eq 0) { L '没有可采集的实例，跳过'; return }

    foreach ($r in $targets) {
        if (-not $r.ConsolePort) { L ($r.Name + '：命令行里没有 -port / -ports，取不到序列号，跳过'); continue }
        $serial = 'emulator-' + $r.ConsolePort
        if ($r.Inferred) { L ('{0}（{1}，实例名是推算的）：' -f $r.Name, $serial) }
        else { L ('{0}（pid {1}，{2}）：' -f $r.Name, $r.Pid, $serial) }
        $base = @('-P', [string]$script:AdbPort, '-s', $serial)

        $png = Join-Path $liveDir ('screen-' + $r.Name + '.png')
        try {
            $res = Invoke-Native -Exe $adb -Arguments ($base + @('exec-out', 'screencap', '-p')) -TimeoutSec 20 -StdoutFile $png -ExtraEnv $adbEnv
            $len = (Get-Item -LiteralPath $png).Length
            $ok = $false
            if ($len -gt 8) {
                $hdr = New-Object byte[] 4
                $fs = [IO.File]::OpenRead($png)
                try { [void]$fs.Read($hdr, 0, 4) } finally { $fs.Dispose() }
                $ok = ($hdr[0] -eq 0x89 -and $hdr[1] -eq 0x50 -and $hdr[2] -eq 0x4E -and $hdr[3] -eq 0x47)
            }
            if ($res.TimedOut) { L '  截图：超时（20 秒），已放弃' }
            elseif ($ok) { L ('  截图：' + (Format-Size $len)) }
            else { L ('  截图：失败，退出码 {0} {1}' -f $res.ExitCode, $res.Stderr.Trim()) }
            if (-not $ok) { Remove-Item -LiteralPath $png -Force -ErrorAction SilentlyContinue }
        } catch {
            L ('  截图出错：' + $_.Exception.Message)
        }

        $cmds = @(
            @{ Tag = 'getprop'; Args = @('shell', 'getprop') },
            @{ Tag = 'window-focus'; Args = @('shell', 'dumpsys', 'window') },
            @{ Tag = 'logcat-crash'; Args = @('logcat', '-d', '-b', 'crash') }
        )
        foreach ($c in $cmds) {
            try {
                $res = Invoke-Native -Exe $adb -Arguments ($base + $c.Args) -TimeoutSec 20 -ExtraEnv $adbEnv
                if ($res.TimedOut) { L ('  {0}：超时（20 秒），已放弃' -f $c.Tag); continue }
                $text = $res.Stdout
                if ($c.Tag -eq 'window-focus') {
                    $text = ((@($text -split "`r?`n") | Where-Object { $_ -match 'mCurrentFocus|mFocusedApp' }) | ForEach-Object { $_.Trim() }) -join "`r`n"
                    foreach ($ln in ($text -split "`r`n")) { if ($ln) { L ('  ' + $ln) } }
                }
                if ($res.Stderr.Trim()) { $text += "`r`n# stderr:`r`n" + $res.Stderr }
                [IO.File]::WriteAllText((Join-Path $liveDir ($r.Name + '-' + $c.Tag + '.txt')), $text, $enc)
                L ('  {0}：退出码 {1}，{2} 字符' -f $c.Tag, $res.ExitCode, $text.Length)
            } catch {
                L ('  {0} 出错：{1}' -f $c.Tag, $_.Exception.Message)
            }
        }
    }
}

# ---------------------------------------------------------------- 6. 写 summary、打包

$elapsed = ((Get-Date) - $script:StartTime).TotalSeconds
Section '收集过程'
L ('收集耗时：{0:N1} 秒' -f $elapsed)
foreach ($t in $script:Timings) { L ('  ' + $t) }
if ($script:Problems.Count -gt 0) {
    L '出错的节：'
    foreach ($p in $script:Problems) { L ('  ' + $p) }
} else {
    L '各节都没出错。'
}
if (-not $script:MasFound) {
    L ''
    L '提示：没找到 AUTO-MAS 安装目录，反馈时请附上 AUTO-MAS 的 debug 文件夹里的日志。'
}

$summaryPath = Join-Path $Stage 'summary.txt'
[IO.File]::WriteAllLines($summaryPath, $script:Summary, (New-Object Text.UTF8Encoding $true))

$zipPath = $null
try {
    if (-not (Test-Path -LiteralPath $OutDir)) { [void](New-Item -ItemType Directory -Path $OutDir -Force) }
    $zipPath = Join-Path $OutDir ('魔改AVD问题信息-' + $script:Stamp + '.zip')
    if (Test-Path -LiteralPath $zipPath) { $zipPath = Join-Path $OutDir ('魔改AVD问题信息-' + $script:Stamp + '-' + $PID + '.zip') }
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    # 不用 ZipFile.CreateFromDirectory：PS 5.1（.NET Framework）下它写出的条目名用反斜杠
    # （avd\mas_7.ini），有的解压工具会把它当成一个带反斜杠的文件名。逐个加条目、统一用 /。
    # 条目名按相对路径逐级拼出来，不截取 FullName（%TEMP% 是 8.3 短名或带 .. 时截取会错位）
    $files = New-Object 'System.Collections.Generic.List[object]'
    $queue = New-Object 'System.Collections.Generic.Queue[object]'
    $queue.Enqueue(@($Stage, ''))
    while ($queue.Count -gt 0) {
        $item = $queue.Dequeue()
        foreach ($f in [IO.Directory]::GetFiles($item[0])) { $files.Add(@($f, ($item[1] + [IO.Path]::GetFileName($f)))) }
        foreach ($d in [IO.Directory]::GetDirectories($item[0])) { $queue.Enqueue(@($d, ($item[1] + [IO.Path]::GetFileName($d) + '/'))) }
    }
    $ordered = @($files | Sort-Object @{ Expression = { if ($_[1] -eq 'summary.txt') { 0 } else { 1 } } }, @{ Expression = { $_[1] } })
    $zip = [IO.Compression.ZipFile]::Open($zipPath, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in $ordered) {
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $f[0], $f[1], [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally {
        $zip.Dispose()
    }
} catch {
    Write-Host ''
    Write-Host ('打包失败：' + $_.Exception.Message) -ForegroundColor Red
    Write-Host ('收集到的文件留在：' + $Stage)
    exit 1
}

if (-not $KeepStage) {
    try { Remove-Item -LiteralPath $Stage -Recurse -Force } catch { Write-Host ('暂存目录没删掉：' + $Stage) }
}

$zipLen = (Get-Item -LiteralPath $zipPath).Length
Write-Host ''
Write-Host '收集完成。' -ForegroundColor Green
Write-Host ('文件：' + $zipPath)
Write-Host ('大小：' + (Format-Size $zipLen))
Write-Host $PrivacyNote -ForegroundColor Yellow
if (-not $script:MasFound) {
    Write-Host '没找到 AUTO-MAS 安装目录，反馈时请附上 AUTO-MAS 的 debug 文件夹里的日志' -ForegroundColor Yellow
}
if ($script:Problems.Count -gt 0) {
    Write-Host ('有 {0} 节没收集成功，原因写在 zip 里的 summary.txt，照样提交就行。' -f $script:Problems.Count)
}
if ($KeepStage) { Write-Host ('暂存目录保留在：' + $Stage) }
try { Start-Process -FilePath 'explorer.exe' -ArgumentList ('/select,"' + $zipPath + '"') } catch { }
exit 0
