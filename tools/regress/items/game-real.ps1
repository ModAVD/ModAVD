# 回归项（tools\regress）：用各游戏自己的自动化脚本实跑。驱动、实例、启动器、采样脚本都在仓库外，位置在本机配置的
# Regress 一节（tools\local.example.psd1）：
#   -Game ark   明日方舟：MAA 集成战略（Regress.Kit 的 w1-maa\maa_roguelike.py，先 StartUp 再肉鸽），Regress.GameAvdHome 的 mas_6
#         r1999 重返未来：1999：M9A 启动游戏 + 智能均衡刷材料（Regress.Kit 的 m9a_run411_load.py），Regress.GameAvdHome 的 mas_7
#         sr    崩坏：星穹铁道：SRC 模拟宇宙（Regress.SrcKit 的 stress.py，日常挂起；SRC 本身在 Regress.SrcDir），Regress.SrAvdHome 的 gfx_t2
#         ef    明日方舟：终末地：MaaEnd（Regress.MaaEndDir 的 maaend-run.ps1 -OpenGame），只跑领取类任务，Regress.GameAvdHome 的 endfield
#   -Rounds on（默认）：只跑装桥那一轮，和用户实例一样默认带桥、按包名分流（不再跑不装桥的对照）。
#           off,on：排查桥本身的开销时才用，先不装桥跑一轮（GL 走 pipe），再装桥跑一轮（asg），两轮对照。
# 开机和 AUTO-MAS 一样带固定开的两项和客体 Vulkan 修复（Regress.VulkanFixLib）。每一轮：启动器（Regress.Launcher）无头开机 →
# 按轮次装 / 卸执行桥（装、卸都要重启客体，之后再冷启动一次，让启动器的开机设置完整；已经装着当前版本就不动）→ 同时起
# emu_monitor.py（Regress.Monitor）采样（30 秒一行：游戏帧率、重启、native crash、ANR、vkerr / devlost / fatal）→
# 驱动跑 -Minutes 分钟 → 先 sync 再关机。装桥那轮跑完不卸桥：测试实例和用户的一样一直带着桥。
# 判过：驱动成功；游戏不重启；无 native crash / ANR / vkerr / devlost / fatal、无新崩溃转储；装桥那轮桥在，
# 走转译层的游戏（arm64）日志里有撤回那一行；装桥那轮帧率中位数不低于上一个生产版的基准（-FpsBaseline）超过容差。
# 同机别的模拟器多于 2 个时帧率只记录、不判（宿主忙时测的数不能和基准比）。
param(
    [Parameter(Mandatory)][ValidateSet('ark', 'r1999', 'sr', 'ef')][string]$Game,
    [string]$Rounds = 'on',
    [int]$Minutes = 15,
    [Parameter(Mandatory)][string]$Sdk,
    [Parameter(Mandatory)][string]$Out,
    [int]$AdbPort = 20089,
    # 上一个生产版装桥那轮的帧率中位数；每个游戏有自己的容差（tol），没写就用 -FpsDropMax
    [string]$FpsBaseline = (Join-Path $PSScriptRoot '..\baselines\fps.json'),
    [double]$FpsDropMax = 0.10,
    # 同机别的模拟器进程多于这个数时，帧率只记录、不判
    [int]$BusyOthers = 2,
    # 客体 Vulkan 修复库（本机配置 Regress.VulkanFixLib）
    [string]$VulkanFixLib,
    # 不开机、不跑：按 -Out 下各轮存好的驱动输出、采样、日志和 round.json 重新判一次（判法改了以后用）
    [switch]$ReJudge
)
$ErrorActionPreference = 'Stop'
[cultureinfo]::CurrentCulture = [cultureinfo]::InvariantCulture
if ($AdbPort -in 5037, 20049, 20050) { throw "adb 端口 $AdbPort 是生产在用的" }
. (Join-Path $PSScriptRoot '..\..\local.ps1')
# 这一项真跑时要的仓库外的东西（-ReJudge 只重新判，都不要）；没配就在这里停
$need = if ($ReJudge) { @() } else {
    @('Launcher', 'Monitor', 'VulkanFixLib') + @{ ark = @('Kit', 'GameAvdHome'); r1999 = @('Kit', 'GameAvdHome')
        sr = @('SrcDir', 'SrcKit', 'SrAvdHome'); ef = @('MaaEndDir', 'GameAvdHome') }[$Game]
}
function Need([string]$k, $v) { if ($need -contains $k) { Use-Local "Regress.$k" $v "Regress.$k" } else { Use-Local "Regress.$k" $v '' -Optional } }
$VulkanFixLib = Need VulkanFixLib $VulkanFixLib
$kit = Need Kit
$srcDir = Need SrcDir
$srcKit = Need SrcKit
$maaEnd = Need MaaEndDir
$root1 = Need GameAvdHome
$cfg = @{
    ark   = @{ Avd = 'mas_6'; Port = 20072; AvdHome = $root1; Memory = 4096; Cores = 4; Pkg = 'com.hypergryph.arknights' }
    r1999 = @{ Avd = 'mas_7'; Port = 20100; AvdHome = $root1; Memory = 6144; Cores = 6; Pkg = 'com.shenlan.m.reverse1999' }
    sr    = @{ Avd = 'gfx_t2'; Port = 20040; AvdHome = (Need SrAvdHome); Memory = 6144; Cores = 8; Pkg = 'com.miHoYo.hkrpg'; WcAsCached = $true }
    ef    = @{ Avd = 'endfield'; Port = 20096; AvdHome = $root1; Memory = 8192; Cores = 6; Pkg = 'com.hypergryph.endfield' }
}[$Game]
$serial = "emulator-$($cfg.Port)"
$adbExe = Join-Path $Sdk 'platform-tools\adb.exe'
$payload = Join-Path $Sdk 'emulator\resources\modavd'
# 随包脚本目录：regress.ps1 的 -PackageScripts，单独跑时同 common.ps1 的默认值（本仓库的 package\）
$bridgeScript = Join-Path $(if ($env:MODAVD_REGRESS_PACKAGE) { $env:MODAVD_REGRESS_PACKAGE } else { Join-Path $PSScriptRoot '..\..\..\package' }) 'modavd-bridge.ps1'
$monitor = Need Monitor
$launcher = Need Launcher
$db = Join-Path $env:LOCALAPPDATA 'Temp\AndroidEmulator'
New-Item -ItemType Directory -Force $Out | Out-Null
$log = Join-Path $Out 'game-real.log'

function Say([string]$m) { $l = "$(Get-Date -Format 'HH:mm:ss') $m"; Add-Content -LiteralPath $log -Value $l -Encoding utf8; Write-Output $l }
function A { & $adbExe -P $AdbPort -s $serial @args 2>&1 | ForEach-Object { "$_" } }
function Qemu { @(Get-CimInstance Win32_Process -Filter "Name like 'qemu-system%'" | Where-Object { $_.CommandLine -match "-avd\s+$([regex]::Escape($cfg.Avd))(\s|$)" }) }
function Dumps { @(Get-ChildItem $db -Recurse -File -Filter *.dmp -ErrorAction SilentlyContinue).Count }
function Prop([string]$name) { ((A shell "getprop $name") -join '').Trim() }
function KillTree([int]$id) { if (Get-Process -Id $id -ErrorAction SilentlyContinue) { & taskkill.exe /PID $id /T /F 2>&1 | Out-Null } }
# 同机别的模拟器进程数（雷电、MuMu、别的 qemu），同 yys.ps1 的 OtherEmulators：帧率很受它影响
function Others {
    @(Get-Process dnplayer, Ld9BoxHeadless, MuMuPlayer, MuMuVMMHeadless, HD-Player -ErrorAction SilentlyContinue).Count +
    @(Get-CimInstance Win32_Process -Filter "Name like 'qemu-system%'" | Where-Object { $_.CommandLine -notmatch "-avd\s+$([regex]::Escape($cfg.Avd))(\s|$)" }).Count
}

function Boot([string]$dir) {
    $env:ANDROID_AVD_HOME = $cfg.AvdHome
    $env:ANDROID_EMU_VK_HIDE_REBAR = '1'   # 10-16 起 AUTO-MAS 对魔改 AVD 固定开
    $p = @{ Avd = $cfg.Avd; ConsolePort = $cfg.Port; AdbPort = $AdbPort; Sdk = $Sdk; Headless = $true; VulkanFix = $true
        VulkanFixLib = $VulkanFixLib; Memory = $cfg.Memory; Cores = $cfg.Cores; LogDir = $dir; Feature = 'VulkanBatchedDescriptorSetUpdate' }
    if ($cfg.WcAsCached) { $p.WcAsCached = $true }
    $o = @(pwsh -NoProfile -File $launcher @p 2>&1 | ForEach-Object { "$_" })
    $o | Add-Content -LiteralPath (Join-Path $dir 'boot.txt') -Encoding utf8
    if (-not (Qemu) -or (Prop sys.boot_completed) -ne '1') { throw "开机失败：$(($o | Select-Object -Last 4) -join ' | ')" }
}
function Stop {
    if (-not (Qemu)) { return }
    A shell sync | Out-Null      # 先落盘再 kill：不先 sync，游戏刚写的文件可能变成 0 字节
    A emu kill | Out-Null
    $t = Get-Date; while ((Qemu) -and ((Get-Date) - $t).TotalSeconds -lt 60) { Start-Sleep 2 }
    if (Qemu) { throw '关机 60 秒内没退出' }
}
# 随包的装桥脚本，用 Windows PowerShell 5.1 跑。不带 -Reboot：要生效时由调用方关机冷启动，和 modavd 一样
# （客体热重启在 WHPX 下可能留下一个不响应 IPI 的 vCPU，10-30 默认装桥项实测内核软锁死）。返回退出码。
# -Root 指向 $Out\bridge-root（里面的 sdk 是联接到被测 SDK 的）：脚本按它找模拟器版本，工作文件（含客体
# build.prop 的备份）落在回归输出里，不落进仓库的 tools\logs。输出先改成 UTF-8，免得中文变问号。
$bridgeRoot = Join-Path $Out 'bridge-root'
if (-not (Test-Path (Join-Path $bridgeRoot 'sdk'))) {
    New-Item -ItemType Directory -Force $bridgeRoot | Out-Null
    New-Item -ItemType Junction -Path (Join-Path $bridgeRoot 'sdk') -Target $Sdk | Out-Null
}
function Bridge([string]$action, [string]$dir) {
    $q = { param($s) "'" + ($s -replace "'", "''") + "'" }
    $cmd = "[Console]::OutputEncoding = [Text.Encoding]::UTF8; & $(& $q $bridgeScript) $action -Serial $serial -AdbPort $AdbPort " +
        "-Adb $(& $q $adbExe) -Payload $(& $q $payload) -Root $(& $q $bridgeRoot); exit `$LASTEXITCODE"
    $o = @(& "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -Command $cmd 2>&1 | ForEach-Object { "$_" })
    $code = $LASTEXITCODE
    $o | Add-Content -LiteralPath (Join-Path $dir "bridge-$action.txt") -Encoding utf8
    # 只返回退出码：Say 会写进输出，混进调用方拿到的值
    $l = "$(Get-Date -Format 'HH:mm:ss')     bridge $action -> $code：$(($o | Where-Object { $_.Trim() } | Select-Object -Last 1))"
    Add-Content -LiteralPath $log -Value $l -Encoding utf8; Write-Host $l
    return $code
}

function StartDriver([string]$dir) {
    $env:MAS_AVD_PORT = "$($cfg.Port)"; $env:PYTHONIOENCODING = 'utf-8'
    switch ($Game) {
        'ark' { $env:MAA_LOOP_MIN = "$Minutes"; $env:MAA_LOG = Join-Path $dir 'maa.jsonl'; $exe = 'py'; $al = @('-3.12', '-u', 'maa_roguelike.py', '--adb-port', "$AdbPort"); $wd = "$kit\w1-maa" }
        'r1999' { $env:M9A_BUDGET_MIN = "$Minutes"; $exe = "$kit\m9a4111\python\python.exe"; $al = @('-u', 'm9a_run411_load.py', '--adb-port', "$AdbPort"); $wd = $kit }
        'sr' {
            # 和内存测试的星铁负载一样：模拟宇宙，日常全部挂起 23 小时（生产做日常）
            $env:STRESS_MIN = "$Minutes"; $env:STRESS_TRIAL_ROUNDS = '0'; $env:STRESS_ROGUE = '1'
            $env:STRESS_NO_DUNGEON = '1'; $env:STRESS_SKIP_RESTART = '1'; $env:STRESS_WATCHDOG_SLACK_H = '0.08'
            $env:STRESS_PARK_UNTIL = (Get-Date).AddHours(23).ToString('yyyy-MM-dd HH:mm:ss'); $env:MAS_SERIAL = $serial
            Remove-Item "$srcKit\src.stop" -ErrorAction SilentlyContinue
            $exe = "$srcDir\toolkit\python.exe"; $al = @("$srcKit\stress.py", '--adb-port', "$AdbPort"); $wd = $srcDir
        }
        'ef' {
            # 只跑领取类（不买、不卖）：拜访好友、基建（帝江号）、日常奖励
            # 输出重定向到文件时 pwsh 按控制台代码页写，中文会丢：先改成 UTF-8
            $exe = 'pwsh'; $wd = $maaEnd
            $al = @('-NoProfile', '-Command', ("[Console]::OutputEncoding = [Text.Encoding]::UTF8; & '$((Join-Path $maaEnd 'maaend-run.ps1') -replace "'", "''")' " +
                "-Serial $serial -AdbServerPort $AdbPort -OpenGame -Tasks VisitFriends,DijiangRewards,DailyRewards " +
                "-TimeoutMinutes $Minutes -OutRoot '$(Join-Path $dir 'maaend')'; exit `$LASTEXITCODE"))
        }
    }
    $p = Start-Process $exe -ArgumentList $al -WorkingDirectory $wd -RedirectStandardOutput (Join-Path $dir 'driver.txt') `
        -RedirectStandardError (Join-Path $dir 'driver.err.txt') -WindowStyle Hidden -PassThru
    $null = $p.Handle
    $p
}

# 驱动自己的结论：$true / 说明
function DriverVerdict([string]$dir, [int]$exitCode) {
    $t = (Get-Content -LiteralPath (Join-Path $dir 'driver.txt') -Raw -ErrorAction SilentlyContinue) + "`n" +
         (Get-Content -LiteralPath (Join-Path $dir 'driver.err.txt') -Raw -ErrorAction SilentlyContinue)
    switch ($Game) {
        'ark' {
            $m = [regex]::Match($t, 'finished: (\d+) chain errors, (\d+) subtask errors')
            if (-not $m.Success) { return $false, 'MAA 没跑到结尾' }
            if ($t -notmatch 'TaskChainCompleted StartUp') { return $false, 'StartUp 没完成' }
            if ([int]$m.Groups[1].Value -gt 0 -or $t -match 'too many subtask errors') { return $false, "MAA $($m.Value)" }
            return $true, "MAA $($m.Value)"
        }
        'r1999' {
            if ($t -notmatch 'done  启动游戏: succeeded') { return $false, 'M9A 启动游戏没成功' }
            $farm = [regex]::Match($t, 'done  智能均衡刷材料: (\w+) in ([\d.]+) min')
            $budgetStop = $t -match '\(budget stop\)'
            if (-not $farm.Success) { return $false, 'M9A 刷材料没跑' }
            if ($farm.Groups[1].Value -ne 'succeeded' -and -not $budgetStop) { return $false, "M9A 刷材料 $($farm.Groups[1].Value)" }
            return $true, "M9A 刷材料 $($farm.Groups[1].Value)$(if ($budgetStop) { '（到时间停）' })，$($farm.Groups[2].Value) 分钟"
        }
        'sr' {
            if ($t -match 'Traceback') { return $false, 'SRC 驱动有 Traceback' }
            if ($t -match 'budget reached') { return $true, 'SRC 模拟宇宙跑满时间' }
            # stress.py 只在调度任务之间看时间：到点时正好在一局模拟宇宙中间，就由它自己的看门狗在「预算 + 余量」时停
            # （guard.py os._exit(3)，日志「watchdog stop」）。这也是跑满了时间，10-30 星铁装桥那轮就是这样。
            if ($t -match 'watchdog stop') { return $true, 'SRC 模拟宇宙跑满时间（一局没打完，由驱动的看门狗按时停）' }
            return $false, "SRC 没跑满时间（退出码 $exitCode）"
        }
        'ef' {
            $m = [regex]::Match($t, '合计：成功 (\d+)，失败 (\d+)，共 (\d+) 项')
            if (-not $m.Success) { return $false, 'MaaEnd 没有结论行' }
            # 日常奖励卡在 DailyEventGoToByRedDot 是 MaaEnd 对活动列表重排的处理问题（10-16 查过），不计入
            $known = @([regex]::Matches($t, '(?m)^\s*\S+\s+\S+\s+失败\s.*失败节点：DailyEventGoToByRedDot')).Count
            $fail = [int]$m.Groups[2].Value - $known
            if ($fail -gt 0 -or [int]$m.Groups[1].Value -eq 0) { return $false, "MaaEnd $($m.Value)" }
            return $true, "MaaEnd $($m.Value)$(if ($known) { "（失败的是日常奖励卡在 DailyEventGoToByRedDot，MaaEnd 已知问题，不计）" })"
        }
    }
}

# 崩溃、ANR、Vulkan 报错直接扫启动器存进本轮目录的日志（emu_monitor 只在它固定的几个目录里找日志，找不到时
# 那几列是空的，10-30 代码审查查出来的）。取最后一次开机的那份（文件名带开机时刻）：模拟器日志开机后就不怎么写了，
# 不能按修改时间筛。规则同 emu_monitor.py；只算这个游戏的进程（Fatal signal / ANR / FATAL EXCEPTION 那一段里有包名），
# 别的进程的记进说明。撤回那一行也从这份 logcat 里数。
function LogScan([string]$dir) {
    $pkg = [regex]::Escape($cfg.Pkg)
    $lc = Get-ChildItem $dir -Filter 'logcat-*.txt' -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
    $em = Get-ChildItem $dir -Filter 'emu-*.log' -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
    $native = 0; $anr = 0; $java = 0; $other = 0; $detach = 0
    if ($lc) {
        $lines = @(Get-Content -LiteralPath $lc.FullName -ErrorAction SilentlyContinue)
        for ($k = 0; $k -lt $lines.Count; $k++) {
            $l = $lines[$k]
            if ($l -match 'Fatal signal \d+') { if ($l -match $pkg) { $native++ } else { $other++ } }
            elseif ($l -match 'ANR in ') { if ($l -match $pkg) { $anr++ } else { $other++ } }
            elseif ($l -match 'FATAL EXCEPTION') {
                $blk = ($lines[$k..([math]::Min($lines.Count - 1, $k + 8))]) -join "`n"
                if ($blk -match "Process: $pkg") { $java++ } elseif ($blk -notmatch 'MaaTouch|minitouch|uiautomator') { $other++ }
            }
            elseif ($l -match "pkg=$pkg not on the gate list") { $detach++ }
        }
    }
    $emuText = if ($em) { Get-Content -LiteralPath $em.FullName -Raw -ErrorAction SilentlyContinue } else { '' }
    [pscustomobject]@{
        Logcat = $(if ($lc) { $lc.Name } else { '' }); EmuLog = $(if ($em) { $em.Name } else { '' })
        Native = $native; Anr = $anr; Java = $java; Other = $other; Detach = $detach
        DevLost = ([regex]::Matches("$emuText", '(?i)DEVICE_LOST|device lost')).Count
        VkErr = ([regex]::Matches("$emuText", 'VK_ERROR')).Count
        Fatal = ([regex]::Matches("$emuText", '\bFATAL\b|abort\(\)|Assertion')).Count
    }
}

# emu_monitor.py 的 samples.csv（30 秒一行）：这里只用帧率和重启。重启只算这个游戏前后两次采样的进程号变了：
# emu_monitor 的 restart 列在前台从桌面换成游戏时也会记 1（游戏第一次启动，不是重启，10-30 1999 那一轮）。
function MonitorStats([string]$dir) {
    $csv = Join-Path $dir 'mon\samples.csv'
    if (-not (Test-Path $csv)) { return $null }
    $rows = @(Import-Csv -LiteralPath $csv -Encoding utf8 | Where-Object { $_.port -eq "$($cfg.Port)" })
    $game = @($rows | Where-Object { $_.pkg -eq $cfg.Pkg })
    $fps = @($game | Where-Object { $_.game_fps -match '^[\d.]+$' -and [double]$_.game_fps -gt 0 } | ForEach-Object { [double]$_.game_fps } | Sort-Object)
    $restart = 0
    for ($k = 1; $k -lt $game.Count; $k++) {
        if ($game[$k].pid -and $game[$k - 1].pid -and $game[$k].pid -ne $game[$k - 1].pid) { $restart++ }
    }
    [pscustomobject]@{
        Rows = $rows.Count; FpsN = $fps.Count; Restart = $restart
        Fps = $(if ($fps.Count) { $fps[[int][math]::Floor(($fps.Count - 1) / 2)] } else { 0 })
    }
}

# 一轮的结论：说明行、问题、帧率。$facts 是跑的时候记下的现场（round.json）：驱动退出码、是否超时、qemu 是否一直在、
# 新转储个数、游戏 ABI、跑完卸桥的退出码。
function Judge([string]$round, [string]$rd, $facts) {
    $probs = @()
    $ok, $what = DriverVerdict $rd ([int]$facts.code)
    if ($facts.timedOut) { $ok = $false; $what = "驱动 $($Minutes + 15) 分钟没结束，已结束它；$what" }
    $s = MonitorStats $rd
    $g = LogScan $rd
    $line = "$round：$what"
    $fps = 0
    if ($s) { $line += ("；帧率中位数 {0:N1}（{1} 个样本）；重启 {2}" -f $s.Fps, $s.FpsN, $s.Restart); $fps = $s.Fps }
    $line += ("；日志：native crash {0}、ANR {1}、Java 崩溃 {2}、vkerr {3}、devlost {4}、fatal {5}" -f
        $g.Native, $g.Anr, $g.Java, $g.VkErr, $g.DevLost, $g.Fatal)
    if ($g.Other) { $line += "（别的进程崩溃 / ANR $($g.Other) 次）" }
    $arm = "$($facts.abi)" -match 'arm'
    if ($round -eq 'on' -and $arm) { $line += "；撤回行 $($g.Detach)" }
    if (-not $ok) { $probs += "$round 驱动：$what" }
    if ($facts.alive -eq $false) { $probs += "$round 模拟器中途退出了" }
    if ([int]$facts.newDumps -gt 0) { $probs += "$round 新崩溃转储 $($facts.newDumps) 个" }
    if (-not $s -or $s.Rows -eq 0) { $probs += "$round 没有采样数据" }
    elseif ($s.Restart -gt 0) { $probs += "$round 游戏进程重启了 $($s.Restart) 次" }
    if (-not $g.Logcat) { $probs += "$round 本轮目录里没有 logcat，崩溃没法查" }
    if (-not $g.EmuLog) { $probs += "$round 本轮目录里没有模拟器日志，Vulkan 报错没法查" }
    if ($g.Native -gt 0 -or $g.Anr -gt 0 -or $g.Java -gt 0 -or $g.VkErr -gt 0 -or $g.DevLost -gt 0 -or $g.Fatal -gt 0) {
        $probs += "$round 日志里有崩溃 / ANR / Vulkan 报错（见上）"
    }
    if ($round -eq 'on' -and $arm -and $g.Detach -eq 0) { $probs += 'on 轮：走转译层的游戏日志里没有撤回那一行' }
    if ($null -ne $facts.uninstall -and [int]$facts.uninstall -ne 0) { $probs += "on 轮跑完卸桥没成（$($facts.uninstall)），这台实例还带着桥" }
    [pscustomobject]@{ Line = $line; Problems = $probs; Fps = $fps }
}

$problems = @(); $notes = @(); $fpsBy = @{}
$roundList = @($Rounds -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
try {
    foreach ($round in $roundList) {
        if ($round -notin 'off', 'on') { throw "轮次只能是 off / on，收到 $round" }
        $rd = Join-Path $Out "$Game-$round"
        if ($ReJudge) {
            if (-not (Test-Path $rd)) { $problems += "$round：没有这一轮的目录 $rd"; continue }
            $fj = Join-Path $rd 'round.json'
            $facts = if (Test-Path $fj) { Get-Content $fj -Raw | ConvertFrom-Json } else {
                # 旧格式（10-30 早先跑的）：没记现场，驱动退出码按 0、ABI 按 logcat 里有没有撤回行猜
                $notes += "$round（旧格式，没有 round.json）"
                [pscustomobject]@{ code = 0; timedOut = $false; alive = $null; newDumps = 0; abi = $(if ((LogScan $rd).Detach) { 'arm64-v8a' } else { '' }); uninstall = $null }
            }
        } else {
            if ($round -eq $roundList[0] -and (Qemu)) { throw "$($cfg.Avd) 已经开着，不动它" }
            New-Item -ItemType Directory -Force $rd | Out-Null
            Say "== $Game 第「$round」轮（$($cfg.Avd)，$Minutes 分钟）"
            Boot $rd
            $nb = Prop ro.dalvik.vm.native.bridge
            if ($round -eq 'on') {
                # 0：装好了（或本来就是当前版本），冷启动后生效；3：这次不是可写开机，或第一次 remount 只关了 verity
                for ($k = 1; $k -le 3; $k++) {
                    $c = Bridge install $rd
                    if ($c -ne 0 -and $c -ne 3) { throw "装桥没成（$c）" }
                    Stop; Boot $rd
                    if ((Prop ro.dalvik.vm.native.bridge) -eq 'libmodavd_bridge.so') { break }
                }
            } elseif ($nb -eq 'libmodavd_bridge.so') {
                $c = Bridge uninstall $rd
                if ($c -ne 0) { throw "卸桥没成（$c）" }
                Stop; Boot $rd
            }
            $nb = Prop ro.dalvik.vm.native.bridge; $gt = Prop ro.boot.hardware.gltransport
            $want = if ($round -eq 'on') { 'libmodavd_bridge.so' } else { 'libndk_translation.so' }
            if ($nb -ne $want) { throw "这一轮的 native bridge 应是 $want，实际 $nb" }
            $abi = ((A shell "dumpsys package $($cfg.Pkg) | grep primaryCpuAbi") -join ' ').Trim() -replace '.*primaryCpuAbi=(\S+).*', '$1'
            Say "  native bridge=$nb gltransport=$(if ($gt) { $gt } else { 'pipe' }) 游戏 ABI=$abi"
            $dump0 = Dumps; $qemu0 = (Qemu)[0].ProcessId

            $mon = Start-Process py -ArgumentList @('-3.12', '-u', $monitor, '--ports', "$($cfg.Port)", '--minutes', "$($Minutes + 2)", '--interval', '30',
                '--shot-every', '300', '--adb-port', "$AdbPort", '--label', "$Game-$round", '--out', (Join-Path $rd 'mon')) `
                -RedirectStandardOutput (Join-Path $rd 'mon.txt') -RedirectStandardError (Join-Path $rd 'mon.err.txt') -WindowStyle Hidden -PassThru
            $drv = StartDriver $rd
            $deadline = (Get-Date).AddMinutes($Minutes + 15)
            $others = Others
            while (-not $drv.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep 10; $others = [math]::Max($others, (Others)) }
            $timedOut = -not $drv.HasExited
            if ($timedOut) { KillTree $drv.Id }
            $code = if ($timedOut) { -1 } else { $drv.ExitCode }
            Start-Sleep 35   # 让采样再记一行
            KillTree $mon.Id
            $facts = [pscustomobject]@{ code = $code; timedOut = $timedOut; alive = (@(Qemu | Where-Object { $_.ProcessId -eq $qemu0 }).Count -gt 0)
                newDumps = (Dumps) - $dump0; abi = $abi; nativeBridge = $nb; glTransport = $gt; uninstall = $null; others = $others }
            Stop
            $facts | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $rd 'round.json') -Encoding utf8
        }
        $j = Judge $round $rd $facts
        Say "  $($j.Line)"
        $notes += $j.Line
        $problems += $j.Problems
        if ($j.Fps -gt 0) { $fpsBy[$round] = $j.Fps }
        if ($round -eq 'on') { $othersOn = $facts.others }
    }
    # 两轮都跑时（排查用）只记录装桥 / 不装桥之比，不判
    if ($fpsBy.ContainsKey('off') -and $fpsBy.ContainsKey('on') -and $fpsBy['off'] -gt 0) {
        $notes += ("帧率 装桥 / 不装桥 = {0:N1} / {1:N1}（{2:P0}）" -f $fpsBy['on'], $fpsBy['off'], ($fpsBy['on'] / $fpsBy['off']))
    }
    # 装桥那轮和上一个生产版的基准比
    if ($fpsBy.ContainsKey('on')) {
        $base = if (Test-Path -LiteralPath $FpsBaseline) { (Get-Content -LiteralPath $FpsBaseline -Raw -Encoding utf8 | ConvertFrom-Json) } else { $null }
        $b = if ($base) { $base.games.$Game } else { $null }
        if (-not $b) { $notes += "没有 $Game 的帧率基准（$FpsBaseline），只记录" }
        else {
            $tol = if ($null -ne $b.tol) { [double]$b.tol } else { $FpsDropMax }
            $ratio = $fpsBy['on'] / [double]$b.fps
            $cmp = "帧率 {0:N1}，上一版（{1}）{2:N1}（{3:P0}，容差 {4:P0}）" -f $fpsBy['on'], $base.version, [double]$b.fps, $ratio, $tol
            if ($null -eq $othersOn) { $notes += "$cmp；这一轮没记同机负载" }
            elseif ([int]$othersOn -gt $BusyOthers) { $notes += "$cmp；同机别的模拟器 $othersOn 个，帧率只记录不判"; $othersOn = $null }
            else { $notes += "$cmp；同机别的模拟器 $othersOn 个" }
            if ($null -ne $othersOn -and $ratio -lt (1 - $tol)) { $problems += ("帧率比上一版低了 {0:P0}（容差 {1:P0}）" -f (1 - $ratio), $tol) }
        }
    }
} catch {
    $problems += "出错（第 $($_.InvocationInfo.ScriptLineNumber) 行）：$($_.Exception.Message)"
    Say $problems[-1]
    if (-not $ReJudge) { try { Stop } catch { Say "关机也出错：$($_.Exception.Message)" } }
}
'REGRESS-RESULT ' + ([ordered]@{ pass = ($problems.Count -eq 0); summary = (($notes + $(if ($problems) { "问题：$($problems -join '；')" })) -join '；') } | ConvertTo-Json -Compress)
