# 回归项：《阴阳师》（arm64 加固，只能靠 arm64 执行桥跑）在装好桥的实例上冷启动、进游戏、测帧。
# 脚本自己只拉起游戏、点游戏自己的引擎更新框；登录页由脚本读像素认（左下角「8+」适龄角标），不经过模型看图。
# 点「进入游戏」交给 MaaYYs 自己的「打开游戏」任务（维护者 10-30 定）。帧只算游戏主进程（父进程是 zygote64）的
# app_time_stats，系统界面的表面不算。
# 通过：桥加载（shim-ctor OK）、有 zram 交换区、到了登录页、游戏进程没有 Fatal signal、每个窗口帧率 ≥ MinFps 且没有超过 1 秒的帧。
# 给了 -MaaTask 就接着用 MaaYYs 真打（maayys_run.py，例如自动御魂壹层 3 场），按 MaaYYs 日志把每场战斗切出来单独判帧率；
# 战斗以外（走菜单、进出战斗读条）只记帧率、限最慢一帧。菜单慢、读条卡 1 秒上下是游戏主线程在 arm64 转译下算不过来
# （10-30 实测：主线程用户态满载、客体其余 CPU 七成空闲、宿主渲染线程两三成），不是图形或磁盘。
param(
    [Parameter(Mandatory)][string]$Sdk,
    # 测试用的 ModAVD 根目录，0 号已装桥、已登录（本机配置 Regress.TestRoot）
    [string]$Root,
    [int]$Index = 0,
    [int]$Windows = 2,
    [int]$Seconds = 30,
    [double]$MinFps = 45,
    # 正常 150–170 秒到登录页。10-29 曾经约五分之一卡死在「正在获取音频列表...」，那不是网络慢，是转译层的
    # fork 死锁（伴生库已修）；再卡就该判失败，不放宽等待去掩盖
    [int]$LoginWaitSec = 420,
    # 没认出登录页时把最后一张截图留在这里；默认 <TempRoot>\regress-yys
    [string]$Out,
    # 测试根的 0 号和生产根目录的 0 号用同一组端口：生产在用就等它关掉
    [int]$PortWaitMin = 180,
    # >0：开机前等同机没有别的模拟器（雷电、MuMu、别的根目录的 qemu）再测，最多这么多分钟；帧率判据只在这时有意义
    [int]$QuietWaitMin = 0,
    # 给了就在庭院测完后用 MaaYYs 跑这个任务（真打战斗），任务跑着时每 $Seconds 秒测一次帧，直到任务结束
    [string]$MaaTask = '',
    # MaaYYs 任务选项，<选项>.<输入>=<值>，例如 自动挑战次数.挑战次数=2
    [string[]]$MaaInput = @(),
    # MaaYYs 选择 / 开关类选项，<选项>=<分支>，例如 是否切换御魂阵容=No
    [string[]]$MaaCase = @(),
    # MaaYYs 的运行目录（interface.json、maafw\、tasks\），仓库外（本机配置 Regress.MaaYYs）
    [string]$MaaRun,
    [int]$MaaDeadline = 900,
    # MaaYYs「打开游戏」（从登录页点「进入游戏」到认出庭院）的时限
    [int]$OpenDeadline = 180,
    # 每场战斗（开打 3 秒后到结算前 3 秒）的帧率下限；战斗段里不许有超过 1 秒的帧
    [double]$BattleFps = 55,
    # 战斗以外（MaaYYs 走菜单、进出战斗读条）最慢一帧的上限：读条时主线程要算 1 秒上下（10-30 实测 0.9–1.3 秒）
    [double]$HitchMaxMs = 2000,
    # 同机别的模拟器进程多于这个数时，庭院和战斗的帧率只记录、不判，读条上限放宽到 $BusyHitchMaxMs。
    # 10-30 同机三台雷电挂机时，同样的战斗每帧多花约 60% 的宿主 CPU 时间（全核负载下降频、超线程共用核心），
    # 庭院 60→50、战斗 57→50 fps、读条最长 2.2 秒
    [int]$BusyOthers = 2,
    [double]$BusyHitchMaxMs = 3000
)
$ErrorActionPreference = 'Stop'
[cultureinfo]::CurrentCulture = [cultureinfo]::InvariantCulture
. "$PSScriptRoot\common.ps1"
$Root = Use-Local Regress.TestRoot $Root '-Root'
if (-not $Out) { $Out = Join-Path (Use-Local TempRoot $null '-Out') 'regress-yys' }
$MaaRun = Use-Local Regress.MaaYYs $MaaRun '-MaaRun'
Add-Type -AssemblyName System.Drawing
# pwsh -File 把 `-MaaCase a=1,b=2` 当成一个字符串
$MaaInput = @($MaaInput | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$MaaCase = @($MaaCase | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$pkg = 'com.netease.onmyoji'

# 截一张屏，认它是哪个界面：login（登录页，左下角「8+」角标亮绿）、region（选服对话框：选中的「已有角色」标签浅色、
# 下面的分区标签蓝色、角色卡浅紫；登录页上这三处都是暗的）、other。$keep 给了就把截图留在那里。
function Screen([string]$keep) {
    $png = if ($keep) { $keep } else { Join-Path ([IO.Path]::GetTempPath()) "regress-yys-$PID.png" }
    $cfg = Get-RootConfig $Root
    cmd /c "`"$(Join-Path $cfg.sdkRoot 'platform-tools\adb.exe')`" -P $($cfg.adbServerPort) -s emulator-$(20000 + 10 * $Index) exec-out screencap -p > `"$png`""
    try {
        $b = [System.Drawing.Bitmap]::FromFile($png)
        try {
            if ($b.Width -ne 1280) { return 'other' }
            $c = $b.GetPixel(60, 600)
            # 「8+」适龄角标：客户端 2.8.85（11-01）起从 x≈60 挪到 x≈165–218，两处都认
            $badge = { param($p) $p.G -gt 150 -and $p.R -lt 150 -and $p.B -lt 110 -and $p.G -gt $p.R + 50 }
            if ((& $badge $c) -or (& $badge $b.GetPixel(190, 580))) { return 'login' }
            $t = $b.GetPixel(285, 160); $u = $b.GetPixel(285, 232); $v = $b.GetPixel(575, 160)
            if ($t.R -gt 170 -and $t.G -gt 190 -and $t.B -gt 190 -and $u.B -gt 120 -and $u.B -gt $u.R + 40 -and $v.B -gt 150) { return 'region' }
            # 登录页上游戏自己弹的两个引擎更新框（10-30）：白框，左按钮白底、右按钮红底。
            #   update：「发现全新的引擎版本！……建议更新」，暂不更新 / 后台下载，按钮中线 y=439
            #   ready：下完以后「已经为您准备好了新版本，是否立即更新」，下次更新 / 立即更新，按钮中线 y=427
            $white = { param($p) $p.R -gt 240 -and $p.G -gt 240 -and $p.B -gt 240 }
            $red = { param($p) $p.R -gt 230 -and $p.G -lt 120 -and $p.B -lt 120 }
            if ((& $white $b.GetPixel(430, 300)) -and (& $white $b.GetPixel(850, 470))) {
                if ((& $white $b.GetPixel(470, 439)) -and (& $red $b.GetPixel(700, 439)) -and (& $red $b.GetPixel(820, 455))) { return 'update' }
                if ((& $white $b.GetPixel(470, 427)) -and (& $red $b.GetPixel(700, 427)) -and (& $red $b.GetPixel(820, 440)) -and (& $white $b.GetPixel(820, 455))) { return 'ready' }
            }
            # 「8+」角标被压暗（对话框盖在登录页上，亮绿 → 约 41,77,23）：还在登录页，只是认不出是哪个框
            if ($c.G -gt 60 -and $c.G -lt 110 -and $c.R -lt 60 -and $c.B -lt 40 -and $c.G -gt $c.R + 25) { return 'dialog' }
            return 'other'
        } finally { $b.Dispose() }
    } catch { return 'other' } finally { if (-not $keep) { Remove-Item -LiteralPath $png -Force -ErrorAction SilentlyContinue } }
}
function OnLogin { (Screen) -eq 'login' }

function OtherEmulators {
    @(Get-Process dnplayer, Ld9BoxHeadless, MuMuPlayer, MuMuVMMHeadless, HD-Player -ErrorAction SilentlyContinue).Count +
    # 自己这台的 qemu 命令行里没有根目录路径，按控制台端口排除
    @(Get-CimInstance Win32_Process -Filter "Name like 'qemu-system%'" | Where-Object { $_.CommandLine -notmatch "-ports\s+$(20000 + 10 * $Index)," }).Count
}

function GamePid {
    ((Su "z=`$(pidof zygote64); ps -A -o PID,PPID,NAME | awk -v z=`"`$z`" '`$2==z && `$3==`"$pkg`" {print `$1}' | head -1") -join '').Trim()
}

function Frames([string]$gp) {
    # 只清、只读 main（app_time_stats 在这里）：crash 缓冲留着给最后查 Fatal signal
    A logcat -b main -c | Out-Null
    Start-Sleep -Seconds $Seconds
    # 每行大约管 1 秒（到上一行为止）；-v epoch 给的是客体的 Unix 时间，战斗测帧拿它和 MaaYYs 日志对时间段
    $stats = (A logcat -d -b main -v epoch) | Select-String "^\s*([\d.]+)\s+$gp\s+\d+\s.*app_time_stats: avg=([\d.]+)ms min=([\d.]+)ms max=([\d.]+)ms count=(\d+)"
    $frames = 0; $ms = 0.0; $worst = 0.0; $long = 0; $lines = @()
    foreach ($m in $stats) {
        $g = $m.Matches[0].Groups; $c = [int]$g[5].Value; $frames += $c; $ms += [double]$g[2].Value * $c; $mx = [double]$g[4].Value
        if ($mx -gt 1000) { $long++ }; if ($mx -gt $worst) { $worst = $mx }
        $lines += [pscustomobject]@{ T = [double]$g[1].Value; N = $c; Max = $mx }
    }
    [pscustomobject]@{ Fps = $frames / $Seconds; Avg = $(if ($frames) { $ms / $frames } else { 0 }); Worst = $worst; Long = $long; Lines = $lines }
}

# 游戏进程常驻内存、整机交换区用量和可用内存（MB）；进程不在了返回空
function GameMem([string]$gp) {
    $o = (Su "grep VmRSS /proc/$gp/status 2>/dev/null; grep -E '^(SwapTotal|SwapFree|MemAvailable):' /proc/meminfo") -join "`n"
    if ($o -notmatch 'VmRSS:\s+(\d+)') { return $null }
    $kb = @{}; foreach ($m in [regex]::Matches($o, '(\w+):\s+(\d+) kB')) { $kb[$m.Groups[1].Value] = [double]$m.Groups[2].Value }
    [pscustomobject]@{ Rss = $kb.VmRSS / 1024; Swap = ($kb.SwapTotal - $kb.SwapFree) / 1024; Avail = $kb.MemAvailable / 1024 }
}

# MaaYYs 日志（宿主本地时间）里每场战斗的时间段（Unix 秒）：开打（锁定阵容时点「挑战」后读条约 3 秒就开打；没锁定时还要
# 「点击准备」）到这一场第一次「点击屏幕继续 / 本次通关时间」（结算）
function FightSpans([string]$mlog) {
    $spans = @(); $start = $null; $inResult = $false
    foreach ($m in (Select-String -LiteralPath $mlog -Pattern '^\[([\d-]+ [\d:.]+)\].*?msg=Node\.Action\.Succeeded.*?"name":"([^"]+)"')) {
        $t = ([DateTimeOffset][datetime]::ParseExact($m.Matches[0].Groups[1].Value, 'yyyy-MM-dd HH:mm:ss.fff', $null)).ToUnixTimeMilliseconds() / 1000.0
        $n = $m.Matches[0].Groups[2].Value
        $isResult = $n -match '点击屏幕继续|通关时间'
        if ($n -match '-开始挑战$') { $start = $t + 3 }
        elseif ($n -match '点击准备$') { $start = $t }
        elseif ($isResult -and -not $inResult -and $start) { $spans += [pscustomobject]@{ A = $start; B = $t }; $start = $null }
        $inResult = $isResult
    }
    $spans
}

# 一段时间内的帧率和最慢一帧：只算整段落在里面的统计行（每行管到上一行为止）
function SpanFrames($lines, [double]$a, [double]$b) {
    $n = 0; $worst = 0.0; $t0 = $null; $t1 = $null; $prev = $null
    foreach ($l in $lines) {
        if ($prev -and $prev.T -ge $a -and $l.T -le $b) {
            if ($null -eq $t0) { $t0 = $prev.T }
            $n += $l.N; $t1 = $l.T; if ($l.Max -gt $worst) { $worst = $l.Max }
        }
        $prev = $l
    }
    if ($null -eq $t0 -or $t1 -le $t0) { return $null }
    [pscustomobject]@{ Fps = $n / ($t1 - $t0); Worst = $worst; Sec = $t1 - $t0 }
}

$old = Set-RootSdk $Root $Sdk
$problems = @(); $notes = @()
try {
    Say "测试根 $Root 换到 $Sdk（原来 $old），开实例 $Index"
    $port = 20000 + 10 * $Index; $tw = Get-Date; $said = $false
    while (Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue) {
        if (-not $said) { Say "控制台端口 $port 被占着（多半是生产的同编号实例），等它空出来，最多 $PortWaitMin 分钟"; $said = $true }
        if (((Get-Date) - $tw).TotalMinutes -ge $PortWaitMin) { throw "端口 $port 等了 $PortWaitMin 分钟还被占着" }
        Start-Sleep 60
    }
    if ($QuietWaitMin -gt 0) {
        $tq = Get-Date; $said = $false
        while ((OtherEmulators) -gt 0) {
            if (-not $said) { Say "同机还有别的模拟器在跑，等它们关掉再测，最多 $QuietWaitMin 分钟"; $said = $true }
            if (((Get-Date) - $tq).TotalMinutes -ge $QuietWaitMin) { throw "等了 $QuietWaitMin 分钟，同机还有别的模拟器" }
            Start-Sleep 60
        }
    }
    # 先把这台实例的执行桥换成这一版的载荷（一样就什么都不做）：modavd bridge 开机、跑包里的装桥脚本、再关机
    $o = M bridge $Index install; $o | Select-Object -Last 2 | ForEach-Object { Say "  bridge: $_" }
    if ($LASTEXITCODE -ne 0) { throw "modavd bridge $Index install 退出码 $LASTEXITCODE" }
    $o = M start $Index; $o | Select-Object -Last 3 | ForEach-Object { Say "  $_" }
    if ($LASTEXITCODE -ne 0) { throw "开机失败（modavd 退出码 $LASTEXITCODE）" }
    $nb = (A logcat -d -b all -s ModAvdNB:V) -join "`n"
    if ($nb -notmatch 'shim-ctor OK') { $problems += '没看到 shim-ctor OK（桥没加载）' }
    $zram = ((Su "grep -c '^/dev/block/zram0 ' /proc/swaps") -join '').Trim()
    if ($zram -ne '1') { $problems += "没有 zram 交换区（$zram）" }
    $nbf = ((A shell 'getprop ro.ndk_translation.flags') -join '').Trim()
    if ($nbf -ne 'accurate-sigsegv') { $notes += "转译层参数 $nbf" }
    # 安卓 13 起，游戏要发通知时系统会弹「允许阴阳师发送通知吗？」，盖住登录页、把左下角角标压暗，就认不出登录页了
    # （10-29 几次「没到登录页」都是它）。设成「不允许」也不行：有热更新要下载时游戏自己会弹「要在通知栏显示下载进度，
    # 请授予通知权限」（10-30）。所以用系统权限命令直接授予并记住，两种弹窗都不出；不碰游戏文件和界面。
    Su "pm grant $pkg android.permission.POST_NOTIFICATIONS; pm set-permission-flags $pkg android.permission.POST_NOTIFICATIONS user-set user-fixed" | Out-Null
    # 点「进入游戏」偶尔会点到服务器那一栏、弹出选服对话框（10-29 好几次、10-30 固定坐标等了 6 秒也碰上一次）。
    # 不去点对话框，结束游戏重新拉起，多等一会儿再点，只重来一次。
    $entered = $false
    for ($try = 1; $try -le 2 -and -not $entered; $try++) {
        if ($try -gt 1) { Su "am force-stop $pkg" | Out-Null; Start-Sleep 3 }
        A shell "monkey -p $pkg -c android.intent.category.LAUNCHER 1" | Out-Null
        $t0 = Get-Date; $login = $false
        while (((Get-Date) - $t0).TotalSeconds -lt $LoginWaitSec) {
            Start-Sleep 5
            $s = Screen
            if ($s -eq 'login') { $login = $true; break }
            # 引擎更新：维护者定了点「后台下载」（10-30），和玩家一样跟着游戏升级，测的是新引擎。下完再点「立即更新」，
            # 游戏重启换引擎，登录页重新等。
            if ($s -eq 'update') { Say '登录页上游戏提示有新的引擎版本，点「后台下载」'; $notes += '游戏提示有新的引擎版本，后台下载'; A shell 'input tap 755 439' | Out-Null }
            if ($s -eq 'ready') {
                Say '新引擎下好了，点「立即更新」'; $notes += '立即更新'
                A shell 'input tap 755 427' | Out-Null
                Start-Sleep 20
                # 游戏换完引擎要是没自己拉起来，就再开一次（这也是「启动游戏」）
                if ((GamePid) -notmatch '^\d+$') { Say '游戏没自己起来，重新拉起'; A shell "monkey -p $pkg -c android.intent.category.LAUNCHER 1" | Out-Null }
                $t0 = Get-Date
            }
        }
        $toLogin = [int]((Get-Date) - $t0).TotalSeconds
        if (-not $login) {
            New-Item -ItemType Directory -Force $Out | Out-Null
            $shot = Join-Path $Out "no-login-$(Get-Date -Format 'HHmmss').png"
            $cfg = Get-RootConfig $Root
            cmd /c "`"$(Join-Path $cfg.sdkRoot 'platform-tools\adb.exe')`" -P $($cfg.adbServerPort) -s emulator-$(20000 + 10 * $Index) exec-out screencap -p > `"$shot`""
            $problems += "$LoginWaitSec 秒内没到登录页（最后的画面 $shot）"
            break
        }
        # 角标先出来，登录页的按钮还在往上滑；马上点会点到服务器那一栏、弹出选服对话框（10-29 好几次因此测成了菜单的 30 帧）
        Start-Sleep $(if ($try -gt 1) { 15 } else { 6 })
        # MaaYYs「打开游戏」：文字识别找「进入游戏」再点，顺带关公告、广告；读条时一直点左上角，到了庭院就点开头像里的
        # 设置、认出「设置」再关掉，停在庭院才算成功（10-30 13:02 实测）。它自己不会停，靠 --deadline 收住。游戏已经
        # 拉起来了，它的 StartApp（monkey）只是切到前台；包名只给 com.netease.onmyoji，不带资源包里写的活动名。
        # 任务失败后重启游戏=No 只是照任务默认选项写明，这条链用不到重启节点。
        Say "到登录页 $toLogin 秒，交给 MaaYYs「打开游戏」点「进入游戏」"
        New-Item -ItemType Directory -Force $Out | Out-Null
        $stamp = Get-Date -Format 'HHmmss'
        Screen (Join-Path $Out "before-enter-$stamp.png") | Out-Null
        $cfg = Get-RootConfig $Root
        $openLog = Join-Path $Out "maa-open-$stamp.txt"
        $argv = @('-I', '-X', 'utf8', (Join-Path $PSScriptRoot 'maayys_run.py'), '--run', $MaaRun,
            '--adb', (Join-Path $cfg.sdkRoot 'platform-tools\adb.exe'), '--port', "$($cfg.adbServerPort)",
            '--address', "emulator-$(20000 + 10 * $Index)", '--task', '打开游戏', '--case', '任务失败后重启游戏=No',
            '--set', "启动游戏.package=$pkg", '--deadline', "$OpenDeadline", '--log-dir', (Join-Path $Out "maa-open-debug-$stamp"))
        $op = Start-Process (Get-Command python).Source -ArgumentList $argv -RedirectStandardOutput $openLog `
            -RedirectStandardError "$openLog.err" -WindowStyle Hidden -PassThru
        # --deadline 管不到连接控制器那一步，外面再兜一层
        if (-not $op.WaitForExit(($OpenDeadline + 120) * 1000)) { $op.Kill($true); $op.WaitForExit() }
        Get-Content -LiteralPath $openLog -Encoding utf8 -ErrorAction SilentlyContinue |
            Where-Object { $_ -match 'task |ALL OK|FAILED|DEADLINE|failed' } | ForEach-Object { Say "  maa: $_" }
        Start-Sleep 5
        # 确认真进了游戏：还在登录页或停在选服对话框，测出来的就是菜单的帧率，不算
        $after = Join-Path $Out "after-enter-$(Get-Date -Format 'HHmmss').png"
        $where = Screen $after
        if ($where -eq 'other') {
            $entered = $true
            # 离开了登录页但 MaaYYs 没认出庭院：可能还在读条或黑屏，后面测的不是庭院的帧，判不通过但接着测完
            if ($op.ExitCode -ne 0) { $problems += "MaaYYs「打开游戏」没认出庭院（退出码 $($op.ExitCode)，见 $openLog）" }
            break
        }
        if ($op.ExitCode -ne 0) { Say "MaaYYs「打开游戏」没跑成（退出码 $($op.ExitCode)，见 $openLog）" }
        $msg = "MaaYYs「打开游戏」结束后还在$(if ($where -eq 'region') { '选服对话框' } elseif ($where -eq 'login') { '登录页' } else { "登录页（盖着对话框：$where）" })，没进游戏（画面 $after）"
        if ($where -ne 'region' -or $try -ge 2) { throw $msg }
        Say "$msg；结束游戏重新拉起，再点一次"
        $notes += '第一次「进入游戏」弹出了选服对话框，重开游戏再交给 MaaYYs'
    }
    if ($entered) {
        $gp = GamePid
        # 帧率很受同机别的模拟器影响（10-29：生产两台雷电挂机时 39–47 fps，空闲时 59–60 fps），记下来对着看
        $others = OtherEmulators
        $busy = $others -gt $BusyOthers
        $notes += "同机别的模拟器进程 $others 个$(if ($busy) { '，帧率只记录不判' })"
        if ($gp -notmatch '^\d+$') { $problems += '进游戏后找不到游戏主进程' }
        else {
            for ($w = 1; $w -le $Windows; $w++) {
                $f = Frames $gp
                Say ("窗口 {0}：{1:N1} fps，平均 {2:N1} ms，最慢 {3:N0} ms，超过 1 秒 {4} 次" -f $w, $f.Fps, $f.Avg, $f.Worst, $f.Long)
                $notes += ("{0:N1} fps / 平均 {1:N1} ms / 最慢 {2:N0} ms" -f $f.Fps, $f.Avg, $f.Worst)
                if ($f.Fps -lt $MinFps -and -not $busy) { $problems += ("窗口 {0} 只有 {1:N1} fps" -f $w, $f.Fps) }
                if ($f.Long) { $problems += "窗口 $w 有 $($f.Long) 次超过 1 秒的帧" }
            }
        }
        if ($MaaTask -and $gp -match '^\d+$') {
            $cfg = Get-RootConfig $Root
            $stamp = Get-Date -Format 'HHmmss'
            $maaLog = Join-Path $Out "maa-$stamp.txt"
            $maaDebug = Join-Path $Out "maa-debug-$stamp"
            $argv = @('-I', '-X', 'utf8', (Join-Path $PSScriptRoot 'maayys_run.py'), '--run', $MaaRun,
                '--adb', (Join-Path $cfg.sdkRoot 'platform-tools\adb.exe'), '--port', "$($cfg.adbServerPort)",
                '--address', "emulator-$(20000 + 10 * $Index)", '--task', $MaaTask, '--deadline', "$MaaDeadline",
                '--log-dir', $maaDebug) + @($MaaInput | ForEach-Object { '--input'; $_ }) + @($MaaCase | ForEach-Object { '--case'; $_ })
            if ($busy) {
                $HitchMaxMs = $BusyHitchMaxMs
                $notes += "宿主忙，战斗帧率只记录，读条 ≤ $HitchMaxMs ms"
            }
            $mem0 = GameMem $gp
            Say "MaaYYs 跑「$MaaTask」：$($MaaInput -join ' ')"
            $mp = Start-Process (Get-Command python).Source -ArgumentList $argv -RedirectStandardOutput $maaLog `
                -RedirectStandardError "$maaLog.err" -WindowStyle Hidden -PassThru
            $bw = @()
            while (-not $mp.HasExited) {
                $f = Frames $gp
                if ($mp.HasExited -and $f.Fps -lt 1) { break }
                $bw += $f
                Say ("MaaYYs 跑着，窗口 {0}：{1:N1} fps，平均 {2:N1} ms，最慢 {3:N0} ms，超过 1 秒 {4} 次" -f $bw.Count, $f.Fps, $f.Avg, $f.Worst, $f.Long)
            }
            $mp.WaitForExit()
            $maaOut = Get-Content -LiteralPath $maaLog -Encoding utf8 -ErrorAction SilentlyContinue
            $maaOut | Where-Object { $_ -match 'task |ALL OK|FAILED|DEADLINE|failed' } | ForEach-Object { Say "  maa: $_" }
            if ($mp.ExitCode -ne 0) { $problems += "MaaYYs「$MaaTask」没跑成（退出码 $($mp.ExitCode)，见 $maaLog）" }
            # 按场切：30 秒的窗口里混着菜单和进出战斗的读条（壹层一场只打 40 秒上下），测不出战斗本身。
            # 一场结算会连点好几下「点击屏幕继续」，连着的只算一场；10-30 困28 卡在切换御魂预设、一场没打，
            # 那次测到的其实是式神录界面的帧，所以没打完一场就不算战斗帧率。
            $mlog = Join-Path $maaDebug 'maafw.log'
            $spans = @(if (Test-Path -LiteralPath $mlog) { FightSpans $mlog })
            $all = @($bw | ForEach-Object { $_.Lines } | Sort-Object T)
            $notes += "MaaYYs 打完 $($spans.Count) 场"
            if ($spans.Count -lt 1) { $problems += 'MaaYYs 一场战斗都没打完，战斗帧率不算数' }
            $k = 0
            foreach ($s in $spans) {
                $k++
                $sf = SpanFrames $all ($s.A + 3) ($s.B - 3)
                if (-not $sf) { $problems += "第 $k 场没测到帧"; continue }
                $notes += ("第 {0} 场 {1:N1} fps、最慢 {2:N0} ms（{3:N0} 秒）" -f $k, $sf.Fps, $sf.Worst, $sf.Sec)
                if ($sf.Fps -lt $BattleFps -and -not $busy) { $problems += ("第 {0} 场只有 {1:N1} fps（要 ≥ {2}）" -f $k, $sf.Fps, $BattleFps) }
                if ($sf.Worst -gt 1000) { $problems += ("第 {0} 场有一帧 {1:N0} ms" -f $k, $sf.Worst) }
            }
            # 战斗以外：只记帧率；最慢一帧（进出战斗的读条）不许超过 $HitchMaxMs
            $n = 0; $sec = 0.0; $worst = 0.0; $prev = $null
            foreach ($l in $all) {
                if ($prev -and -not @($spans | Where-Object { $l.T -gt $_.A -and $prev.T -lt $_.B }).Count) {
                    $n += $l.N; $sec += $l.T - $prev.T; if ($l.Max -gt $worst) { $worst = $l.Max }
                }
                $prev = $l
            }
            if ($sec -gt 0) {
                $notes += ("战斗以外（菜单、读条）{0:N1} fps、最慢 {1:N0} ms（{2:N0} 秒）" -f ($n / $sec), $worst, $sec)
                if ($worst -gt $HitchMaxMs) { $problems += ("战斗以外有一帧 {0:N0} ms（要 ≤ {1}）" -f $worst, $HitchMaxMs) }
            }
            # 内存：游戏进程常驻、交换区用量、可用内存，MaaYYs 前后各一次（长时间跑看有没有一直涨）
            $mem1 = GameMem $gp
            if ($mem0 -and $mem1) {
                $notes += ("内存：游戏常驻 {0:N0} → {1:N0} MB，交换区 {2:N0} → {3:N0} MB，可用 {4:N0} → {5:N0} MB" -f `
                    $mem0.Rss, $mem1.Rss, $mem0.Swap, $mem1.Swap, $mem0.Avail, $mem1.Avail)
            } elseif (-not $mem1) { $problems += 'MaaYYs 跑完后游戏进程不在了' }
        }
        $sig = @((A logcat -d -b crash) | Select-String -SimpleMatch 'Fatal signal').Count
        if ($sig) { $problems += "crash 缓冲里有 $sig 条 Fatal signal" }
    }
} catch {
    $problems += "出错：$($_.Exception.Message)"
} finally {
    $s = M stop $Index; $s | Select-Object -Last 1 | ForEach-Object { Say "  $_" }
    Set-RootSdk $Root $old | Out-Null
}
$summary = "到登录页 $toLogin 秒；进游戏后 $($notes -join '；')"
if ($problems) { $summary += "；问题：$($problems -join '；')" }
Result ($problems.Count -eq 0) $summary
exit 0
