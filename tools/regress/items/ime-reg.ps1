# 回归项（tools\regress）：宿主输入法通道 20 项，mas-35 回归用的那份（修好 $S/$s 撞名之后）。
# 10-30 加了 -Attach 那组参数给 ime-pkg.ps1 用；不带这些参数时和原来做的一模一样（套件的 ime 项就这么跑）。
# 结论行「汇总：PASS n，FAIL m」由套件的正则判。
# 带窗口起一次性冒烟机 imesmoke（console 20110，已装 org.modavd.ime 和测试 APK；ANDROID_AVD_HOME 是本机配置的
# Regress.ImeAvdHome，启动器是 Regress.Launcher），经控制台 `ime` 命令驱动客体输入法，在测试 APK 的 ImeTest 日志里
# 核对文字；最后先 sync 再 emu kill。
# 用法：pwsh -NoProfile -File tools\regress\items\ime-reg.ps1 -Sdk <SDK 根目录>
# -Attach：对一台已经带窗口开着的实例跑同样的 20 项，不开机、不关机（关机归调用方）。同时给 -AdbPort、-Serial、
#   -EmulatorLog（这次开机的模拟器日志），-AdbExe 可换；-TestApk 给了就先装上测试 APK、跑完卸掉；-Sdk 只用来记版本号。
#   「模拟器日志」一项这时只看开测以后：开机时客体 Vulkan 修复重启 zygote，输入法会断开再连上一次，那次不算。
param(
    [Parameter(Mandatory)][string]$Sdk,
    # 默认 <TempRoot>\regress-ime
    [string]$Out,
    [switch]$Attach,
    # 不给就用 -Sdk 的 platform-tools\adb.exe（启动器起 adb server 用的也是它）
    [string]$AdbExe,
    [int]$AdbPort = 20089,
    [string]$Serial = 'emulator-20110',
    [string]$EmulatorLog,
    [string]$TestApk
)
$ErrorActionPreference = 'Stop'
[cultureinfo]::CurrentCulture = [cultureinfo]::InvariantCulture
$attachOnly = @('AdbExe', 'AdbPort', 'Serial', 'EmulatorLog', 'TestApk') | Where-Object { $PSBoundParameters.ContainsKey($_) }
if (-not $Attach -and $attachOnly) { throw "-$($attachOnly -join '、-') 只能和 -Attach 一起用" }
if ($Attach -and -not ($PSBoundParameters.ContainsKey('AdbPort') -and $PSBoundParameters.ContainsKey('Serial') -and $EmulatorLog)) { throw '-Attach 要同时给 -AdbPort、-Serial、-EmulatorLog' }
if ($AdbPort -in 5037, 20049, 20050) { throw "adb 端口 $AdbPort 是生产用的，不碰" }
. (Join-Path $PSScriptRoot '..\..\local.ps1')
if (-not $Out) { $Out = Join-Path (Use-Local TempRoot $null '-Out') 'regress-ime' }
if (-not $AdbExe) { $AdbExe = Join-Path $Sdk 'platform-tools\adb.exe' }
New-Item -ItemType Directory -Force $Out, "$Out\emu" | Out-Null
$log = "$Out\ime-reg.log"
function L([string]$m) { $l = "$(Get-Date -Format 'HH:mm:ss') $m"; Add-Content -LiteralPath $log -Value $l -Encoding utf8; Write-Output $l }
$adb = $AdbExe
# 变量名不要和别的变量只差大小写（PowerShell 不分大小写，又是动态作用域）：10-27 第一次跑时 $S 被状态变量 $s 覆盖，
# adb 丢了 -P/-s，落到默认的 5037 上。现在每次调用前都核对目标（和脚本作用域里记下的端口、序列号比），不对就直接抛异常。
$AdbTarget = @('-P', "$AdbPort", '-s', $Serial)
$script:ImeRegPort = "$AdbPort"; $script:ImeRegSerial = $Serial
function A {
    if ($AdbTarget.Count -ne 4 -or $AdbTarget[1] -ne $script:ImeRegPort -or $AdbTarget[3] -ne $script:ImeRegSerial) { throw 'adb 目标丢了，停止' }
    & $adb $AdbTarget[0] $AdbTarget[1] $AdbTarget[2] $AdbTarget[3] @args 2>&1 | ForEach-Object { "$_" }
}
function Get-Qemu { Get-CimInstance Win32_Process -Filter "Name like 'qemu-system%'" | Where-Object { $_.CommandLine -match '-avd\s+imesmoke(\s|$)' } }
function Status { $h = @{}; foreach ($l in (A emu ime status)) { if ($l -match '^(\w+)=(.*)$') { $h[$matches[1]] = $matches[2].Trim() } }; $h }
$script:pass = 0; $script:fail = 0
function Check([string]$name, [bool]$ok, [string]$detail) {
    if ($ok) { $script:pass++; L "PASS $name" } else { $script:fail++; L "FAIL $name：$detail" }
}
function LastState([string]$id) {
    $o = $null
    foreach ($l in (A logcat -d -s ImeTest:I)) { if ($l -match 'ImeTest\s*:\s*(\{.*\})\s*$') { $j = $matches[1] | ConvertFrom-Json; if ($j.id -eq $id) { $o = $j } } }
    $o
}
function WaitText([string]$id, [string]$want, [int]$sec = 6) {
    $t = (Get-Date).AddSeconds($sec)
    do { $o = LastState $id; if ($o -and $o.text -ceq $want) { return $o }; Start-Sleep -Milliseconds 300 } while ((Get-Date) -lt $t)
    $o
}
function WaitStatus([string]$key, [string]$want, [int]$sec = 10) {
    $t = (Get-Date).AddSeconds($sec)
    do { $st = Status; if ($st[$key] -eq $want) { return $st }; Start-Sleep -Milliseconds 300 } while ((Get-Date) -lt $t)
    $st
}
function Show($o) { if ($o) { $o | ConvertTo-Json -Compress } else { '(无记录)' } }
function Stop-Emu {
    if (-not (Get-Qemu)) { return }
    A shell sync | Out-Null
    A emu kill | Out-Null
    $t = Get-Date; while ((Get-Qemu) -and ((Get-Date) - $t).TotalSeconds -lt 60) { Start-Sleep 2 }
    L "关机（先 sync 再 emu kill）：$(if (Get-Qemu) { '60 秒内没退出' } else { '已退出' })"
}

if ($Attach) {
    L "开始（-Attach：已开着的 $Serial，adb server $AdbPort）；SDK $Sdk（$((Select-String -Path "$Sdk\emulator\source.properties" -Pattern 'BuildId').Line)）"
    $devState = ((A get-state) -join ' ').Trim()
    if ($devState -ne 'device') { L "实例没开着：$devState"; exit 1 }
}
else {
    if (Get-Qemu) { L 'imesmoke 已经开着，不动它，退出'; exit 2 }
    L "开始；SDK $Sdk（$((Select-String -Path "$Sdk\emulator\source.properties" -Pattern 'BuildId').Line)）"
    $env:ANDROID_AVD_HOME = Use-Local Regress.ImeAvdHome $null 'Regress.ImeAvdHome'
    $launcher = Use-Local Regress.Launcher $null 'Regress.Launcher'
    cmd /c "pwsh -NoProfile -File `"$launcher`" -Avd imesmoke -ConsolePort 20110 -AdbPort 20089 -Sdk `"$Sdk`" -LogDir `"$Out\emu`" -NoLogcat > `"$Out\launcher.log`" 2>&1"
    Get-Content "$Out\launcher.log" | Select-String 'SDK:|booted|adb server' | ForEach-Object { L "launcher: $($_.Line.Trim())" }
    if (-not (Get-Qemu)) { L '开机失败'; exit 1 }
}
$testApkIn = $false
try {
    if ($Attach) {
        $emuLog = $EmulatorLog
        if ($TestApk) {
            $r = (A install -r $TestApk) -join ' '
            L "装测试 APK $TestApk：$r"
            if ($r -notmatch '\bSuccess\b') { throw "测试 APK 没装上：$r" }
            $testApkIn = $true
        }
        # 开测前模拟器日志已有的行数：最后只数这之后的连接和断开
        $emuLogLines0 = @(Get-Content -LiteralPath $emuLog).Count
    }
    else {
        $emuLog = Get-ChildItem "$Out\emu" -File -Filter 'emu-*.log' | Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
    }
    Check '服务注册日志' ([bool](Select-String -Path $emuLog -SimpleMatch 'modavd_ime: host input method pipe service registered')) $emuLog
    $ime = ((A shell settings get secure default_input_method) -join '').Trim()
    if ($ime -ne 'org.modavd.ime/.HostImeService') { L "默认输入法是 $ime，切回宿主输入法"; A shell ime set org.modavd.ime/.HostImeService | Out-Null }
    A shell setprop log.tag.ModAVDIme DEBUG | Out-Null
    A logcat -c | Out-Null

    # 文本框：连接、文字模式、光标
    A shell am start -n org.modavd.ime.testapp/.MainActivity --es focus text --ez clear true | Out-Null
    $st = WaitStatus 'active' '1' 20
    Check '文本框：connected=1 version=1 active=1' ($st.connected -eq '1' -and $st.version -eq '1' -and $st.active -eq '1') ($st | ConvertTo-Json -Compress)
    Check '文本框：inputType=1' ($st.inputType -eq '1') "inputType=$($st.inputType)"
    $st = WaitStatus 'textmode' '1'
    Check '带窗口：textmode=1' ($st.textmode -eq '1') "textmode=$($st.textmode)"
    $t = (Get-Date).AddSeconds(6); do { $st = Status; if ($st.cursor -and $st.cursor -ne 'none') { break }; Start-Sleep -Milliseconds 300 } while ((Get-Date) -lt $t)
    Check '光标矩形已收到' ($st.cursor -match '^\d+,\d+,\d+,\d+$') "cursor=$($st.cursor)"

    # 上屏：中文、瑞典语字母、首个空格之后的空格保留、4 字节 UTF-8
    $want = 'Hej på dig 你好'
    A emu ime commit $want | Out-Null
    $o = WaitText 'text' $want
    Check '上屏 中文 + å' ($o.text -ceq $want) (Show $o)
    A emu ime commit ' 🙂' | Out-Null
    $want = 'Hej på dig 你好 🙂'
    $o = WaitText 'text' $want
    Check '上屏 前导空格 + emoji' ($o.text -ceq $want) (Show $o)
    $base = $want

    # 组字、结束组字
    A emu ime preedit abc | Out-Null
    $o = WaitText 'text' ($base + 'abc')
    Check '组字 abc' ($o.text -ceq ($base + 'abc') -and $o.composing -and $o.composing[0] -eq $base.Length -and $o.composing[1] -eq ($base.Length + 3)) (Show $o)
    A emu ime finish | Out-Null
    $t = (Get-Date).AddSeconds(5); do { $o = LastState 'text'; if ($null -eq $o.composing) { break }; Start-Sleep -Milliseconds 300 } while ((Get-Date) -lt $t)
    Check '结束组字' ($o.text -ceq ($base + 'abc') -and $null -eq $o.composing) (Show $o)

    # 按键、删除
    A emu ime key 67 | Out-Null
    $o = WaitText 'text' ($base + 'ab')
    Check '按键 DEL(67)' ($o.text -ceq ($base + 'ab')) (Show $o)
    A emu ime delete 2 0 | Out-Null
    $o = WaitText 'text' $base
    Check '删除 2 0' ($o.text -ceq $base) (Show $o)
    A emu ime key 29 0x1000 | Out-Null
    $t = (Get-Date).AddSeconds(5); do { $o = LastState 'text'; if ($o.selStart -eq 0 -and $o.selEnd -eq $base.Length) { break }; Start-Sleep -Milliseconds 300 } while ((Get-Date) -lt $t)
    Check '按键 Ctrl+A 全选' ($o.selStart -eq 0 -and $o.selEnd -eq $base.Length) (Show $o)
    A emu ime commit x | Out-Null
    $o = WaitText 'text' 'x'
    Check '全选后上屏替换' ($o.text -ceq 'x') (Show $o)
    $r = (A emu ime delete -1 0) -join ' '
    Check '负数删除被拒' ($r -match 'KO') $r

    # 密码框
    A shell am start -n org.modavd.ime.testapp/.MainActivity --es focus password | Out-Null
    $st = WaitStatus 'inputType' '129'
    Check '密码框：inputType=129 active=1 textmode=1' ($st.inputType -eq '129' -and $st.active -eq '1' -and $st.textmode -eq '1') ($st | ConvertTo-Json -Compress)
    A emu ime commit pw | Out-Null
    $o = WaitText 'password' 'pw'
    Check '密码框上屏' ($o.text -ceq 'pw') (Show $o)

    # 非文本控件：文字模式应退出
    A shell am start -n org.modavd.ime.testapp/.MainActivity --es focus raw | Out-Null
    $st = WaitStatus 'textmode' '0'
    Check '非文本控件：active=0 textmode=0' ($st.active -eq '0' -and $st.textmode -eq '0') ($st | ConvertTo-Json -Compress)

    # 回到文本框
    A shell am start -n org.modavd.ime.testapp/.MainActivity --es focus text | Out-Null
    $st = WaitStatus 'textmode' '1'
    Check '回到文本框：textmode=1' ($st.active -eq '1' -and $st.textmode -eq '1') ($st | ConvertTo-Json -Compress)

    if ($Attach) {
        # 坏帧整次开机都不该有；连接、断开只数开测以后的（开机时那次断开重连不算），整次开机至少连上过一次
        $emuLogNew = @(Get-Content -LiteralPath $emuLog | Select-Object -Skip $emuLogLines0)
        $bad = @(Select-String -Path $emuLog -Pattern 'modavd_ime: .*malformed|host IME: replacement').Count + @($emuLogNew | Select-String -Pattern 'modavd_ime: .*disconnected').Count
        $conn = @($emuLogNew | Select-String -SimpleMatch 'modavd_ime: guest input method connected').Count
        $connAll = @(Select-String -Path $emuLog -SimpleMatch 'modavd_ime: guest input method connected').Count
        Check '模拟器日志：开测后没有重连、无坏帧/断开' ($bad -eq 0 -and $conn -eq 0 -and $connAll -ge 1) "开测后 connected=$conn，bad=$bad，整次开机 connected=$connAll"
    }
    else {
        $bad = @(Select-String -Path $emuLog -Pattern 'modavd_ime: .*(malformed|disconnected)|host IME: replacement').Count
        $conn = @(Select-String -Path $emuLog -SimpleMatch 'modavd_ime: guest input method connected').Count
        Check '模拟器日志：1 次连接、无坏帧/断开' ($bad -eq 0 -and $conn -eq 1) "connected=$conn bad=$bad"
    }
    $crash = @(A logcat -d -b crash | Where-Object { $_ -match 'org\.modavd' }).Count
    Check '客体无 org.modavd 崩溃' ($crash -eq 0) "crash lines=$crash"
    A logcat -d -v threadtime -s ModAVDIme:V ImeTest:V | Set-Content -LiteralPath "$Out\logcat-ime.txt" -Encoding utf8
}
catch { $script:fail++; L "FAIL 脚本异常：$($_.Exception.Message)" }
finally {
    if ($Attach) {
        if ($testApkIn) { L "卸掉测试 APK：$((A uninstall org.modavd.ime.testapp) -join ' ')" }
    }
    else { Stop-Emu }
}
L "汇总：PASS $script:pass，FAIL $script:fail"
exit ([int]($script:fail -gt 0))
