# 回归项：arm64 执行桥的装 / 卸全流程，在一台新建的实例上，用 Windows PowerShell 5.1 跑随包的 modavd-bridge.ps1（用户就是这么跑的）。
#  1. 不装桥的实例零影响：/system/lib64 没有我们的文件、ModAvdNB 零日志、属性是 libndk_translation.so
#  2. 照常开机（不是可写系统镜像）时 install：什么都不改，放开机标记，退出码 3
#  3. 关机再开（模拟器看到标记按可写系统镜像开）后 install -Reboot：第一次 remount 自动重启、装好、核对全过
#  4. 再跑一次 install：「已安装且是当前版本」，不重启（uptime 接着涨）
#  5. 关机冷启动：桥还在（shim-ctor OK）、有 zram 交换区，verify 全过
#  6. uninstall -Reboot：核对全过（属性回到 stock、三个文件删掉、开机标记删掉）
# 跑完删掉这台实例（-Keep 留着）。
param(
    [Parameter(Mandatory)][string]$Sdk,
    # 测试用的 ModAVD 根目录（本机配置 Regress.TestRoot）
    [string]$Root,
    [int]$Index = 6,
    # 默认取 common.ps1 的随包脚本目录
    [string]$BridgeScript,
    [switch]$Keep
)
$ErrorActionPreference = 'Stop'
[cultureinfo]::CurrentCulture = [cultureinfo]::InvariantCulture
. "$PSScriptRoot\common.ps1"
$Root = Use-Local Regress.TestRoot $Root '-Root'
if (-not $BridgeScript) { $BridgeScript = Join-Path $script:PackageScripts 'modavd-bridge.ps1' }
$payload = Join-Path $Sdk 'emulator\resources\modavd'

function Bridge([string]$action, [switch]$Reboot) {
    $cfg = Get-RootConfig $Root
    # 不传 -AdbPort：脚本自己从测试根的 mas-avd.json 读，和用户照「怎么更新」敲命令时一样
    $argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Resolve-Path $BridgeScript).Path, $action, '-Root', $Root,
        '-Adb', (Join-Path $cfg.sdkRoot 'platform-tools\adb.exe'), '-Payload', $payload, '-Serial', "emulator-$(20000 + 10 * $Index)")
    if ($Reboot) { $argv += '-Reboot' }
    $out = & "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" @argv 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    Say "  modavd-bridge $action$(if ($Reboot) { ' -Reboot' }) -> 退出码 $code：$(($out | Where-Object { $_.Trim() } | Select-Object -Last 1))"
    [pscustomobject]@{ Code = $code; Text = ($out -join "`n") }
}
function Uptime { [double](((A shell 'cat /proc/uptime') -join '') -split '\s+')[0] }
function Stock {
    $o = Su 'getprop ro.dalvik.vm.native.bridge; ls /system/lib64 | grep -c libmodavd; logcat -d -b all -s ModAvdNB:V | grep -c shim-ctor'
    [pscustomobject]@{ Prop = $o[0].Trim(); Files = $o[1].Trim(); NbLog = $o[2].Trim() }
}

$problems = @(); $notes = @(); $created = $false
$marker = Join-Path $Root "avd\mas_$Index.avd\modavd-writable-system"
$old = Set-RootSdk $Root $Sdk
try {
    if (-not (Test-Path "$payload\libmodavd_bridge.so")) { throw "SDK 里没有执行桥载荷：$payload" }
    M info $Index | Out-Null
    if ($LASTEXITCODE -eq 0) { throw "测试根里已经有实例 $Index，换 -Index 或先删掉它" }
    $o = M create --index $Index --title 回归执行桥 --memory 4G --cpu 4 --no-bridge
    if ($LASTEXITCODE -ne 0) { throw "新建失败：$($o -join ' ')" }
    $created = $true
    M start $Index --no-bridge | Select-Object -Last 1 | ForEach-Object { Say "  $_" }
    if ($LASTEXITCODE -ne 0) { throw "开机失败（modavd 退出码 $LASTEXITCODE）" }

    $s = Stock
    if ($s.Prop -ne 'libndk_translation.so' -or $s.Files -ne '0' -or $s.NbLog -ne '0') { $problems += "没装桥的实例不干净：属性 $($s.Prop)、文件 $($s.Files)、ModAvdNB $($s.NbLog)" }
    else { $notes += '没装桥时零影响' }

    $r = Bridge install
    if ($r.Code -ne 3 -or -not (Test-Path $marker)) { $problems += "照常开机时 install 应退出码 3 并放标记（实际 $($r.Code)，标记 $(Test-Path $marker)）" }
    elseif ((Stock).Prop -ne 'libndk_translation.so') { $problems += '照常开机时 install 改了属性' }
    else { $notes += '照常开机时 install 不动客体、放标记、退出码 3' }

    M stop $Index | Out-Null
    M start $Index --no-bridge | Select-Object -Last 1 | ForEach-Object { Say "  $_" }
    $r = Bridge install -Reboot
    if ($r.Code -ne 0 -or $r.Text -notmatch '装好了，核对全过') { $problems += "install -Reboot 没成（退出码 $($r.Code)）" } else { $notes += 'install -Reboot 核对全过' }
    $cfgIni = Join-Path $Root "avd\mas_$Index.avd\config.ini"
    if (-not (Select-String -LiteralPath $cfgIni -Pattern '^hw\.gltransport=asg$' -Quiet)) { $problems += '装桥后 config.ini 没有 hw.gltransport=asg' } else { $notes += 'GL 传输设成 asg' }

    $u0 = Uptime
    $r = Bridge install
    $u1 = Uptime
    if ($r.Code -ne 0 -or $r.Text -notmatch '已安装且是当前版本' -or $u1 -lt $u0) { $problems += "重复 install 不是「已安装且是当前版本」或重启了（退出码 $($r.Code)，uptime $u0 -> $u1）" }
    else { $notes += '重复 install 不动' }

    M stop $Index | Out-Null
    M start $Index --no-bridge | Select-Object -Last 1 | ForEach-Object { Say "  $_" }
    $zram = ((Su "grep -c '^/dev/block/zram0 ' /proc/swaps") -join '').Trim()
    $r = Bridge verify
    $asgOn = ((A shell 'getprop ro.boot.hardware.gltransport') -join '').Trim()
    if ($r.Code -ne 0 -or $zram -ne '1') { $problems += "冷启动后 verify 退出码 $($r.Code)、zram $zram" } else { $notes += '冷启动后桥在、zram 在' }
    if ($asgOn -ne 'asg') { $problems += "冷启动后实际 GL 传输是「$asgOn」，不是 asg" } else { $notes += '冷启动后实际走 asg' }

    $r = Bridge uninstall -Reboot
    $s = Stock
    if (Select-String -LiteralPath $cfgIni -Pattern '^hw\.gltransport=' -Quiet) { $problems += '卸桥后 config.ini 还留着 hw.gltransport' }
    if ($r.Code -ne 0 -or $s.Prop -ne 'libndk_translation.so' -or $s.Files -ne '0' -or (Test-Path $marker)) {
        $problems += "uninstall -Reboot 没卸干净（退出码 $($r.Code)，属性 $($s.Prop)，文件 $($s.Files)，标记 $(Test-Path $marker)）"
    } else { $notes += 'uninstall 卸干净' }
} catch {
    $problems += "出错：$($_.Exception.Message)"
} finally {
    M stop $Index | Select-Object -Last 1 | ForEach-Object { Say "  $_" }
    if ($created -and -not $Keep) { Remove-TestInstance $Index }
    Set-RootSdk $Root $old | Out-Null
}
$summary = $notes -join '；'
if ($problems) { $summary += "；问题：$($problems -join '；')" }
Result ($problems.Count -eq 0) $summary
exit 0
