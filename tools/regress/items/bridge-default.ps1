# 回归项：新建实例默认装执行桥（0.36.0 起），以及 modavd bridge 的卸 / 装。在测试根临时建一台实例，跑完删掉。
#  1. modavd create（不带 --no-bridge）→ 实例目录里有开机标记；
#  2. 第一次 modavd start：做完首次初始化后自己装桥（中间关机冷启动一两次）→ ro.dalvik.vm.native.bridge 是我们的、
#     转发库 shim-ctor OK、伴生库带撤回导出（detach=）；
#  3. 冷启动（stop + start）：桥还在、GL 传输是 asg；
#  4. modavd bridge <i> uninstall：回到原转译层、开机标记没了；modavd bridge <i> install：又装上；
#  5. 关机、删实例。
param(
    [Parameter(Mandatory)][string]$Sdk,
    # 测试用的 ModAVD 根目录（本机配置 Regress.TestRoot）
    [string]$Root,
    [int]$Index = 7   # 执行桥全流程项用 6、扩容项用 3
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"
$Root = Use-Local Regress.TestRoot $Root '-Root'
$script:Root = $Root; $script:Index = $Index
$problems = @(); $notes = @(); $created = $false
$marker = Join-Path $Root "avd\mas_$Index.avd\modavd-writable-system"
function Prop { ((A shell 'getprop ro.dalvik.vm.native.bridge') -join '').Trim() }

$old = Set-RootSdk $Root $Sdk
try {
    if (Test-Path (Join-Path $Root "avd\mas_$Index.avd")) { throw "实例 $Index 已存在（上次留下的？）" }
    $o = M create --index $Index --title 回归默认装桥 --memory 4G --cpu 4
    if ($LASTEXITCODE -ne 0) { throw "新建失败：$($o -join ' ')" }
    $created = $true
    if (Test-Path -LiteralPath $marker) { $notes += '新建后有开机标记' } else { $problems += '新建后没有开机标记' }

    $t0 = Get-Date
    $o = M start $Index; $o | Select-Object -Last 2 | ForEach-Object { Say "  $_" }
    if ($LASTEXITCODE -ne 0) { throw "第一次开机失败（$LASTEXITCODE）" }
    $first = [int]((Get-Date) - $t0).TotalSeconds
    $p = Prop
    $logs = (A logcat -d -b all -s ModAvdNB:V) -join "`n"
    if ($p -eq 'libmodavd_bridge.so') { $notes += "第一次开机装好了桥（共 $first 秒）" } else { $problems += "第一次开机后 native bridge 是 $p" }
    if ($logs -notmatch 'shim-ctor OK') { $problems += '没看到 shim-ctor OK' }
    # bionic 的 %p 把 NULL 打成 0x0：要求非零
    if ($logs -notmatch 'detach=0x0*[1-9a-f][0-9a-f]*') { $problems += '伴生库没有撤回导出（detach=）' }
    $info = (M info $Index) -join "`n"
    if ($info -notmatch '初始化|initialized') { $notes += '（info 里没找到首次初始化字样）' }

    M stop $Index | Out-Null
    $o = M start $Index; $o | Select-Object -Last 1 | ForEach-Object { Say "  $_" }
    $gt = ((A shell 'getprop ro.boot.hardware.gltransport') -join '').Trim()
    if ((Prop) -eq 'libmodavd_bridge.so' -and $gt -eq 'asg') { $notes += '冷启动后桥在、走 asg' } else { $problems += "冷启动后 native bridge=$(Prop) gltransport=$gt" }
    M stop $Index | Out-Null

    $o = M bridge $Index uninstall; $o | Select-Object -Last 2 | ForEach-Object { Say "  uninstall: $_" }
    if ($LASTEXITCODE -ne 0) { $problems += "modavd bridge uninstall 退出码 $LASTEXITCODE" }
    if (Test-Path -LiteralPath $marker) { $problems += '卸桥后开机标记还在' }
    M start $Index --no-bridge | Out-Null
    if ((Prop) -eq 'libndk_translation.so') { $notes += 'modavd bridge uninstall 卸干净' } else { $problems += "卸桥后冷启动 native bridge=$(Prop)" }
    M stop $Index | Out-Null

    $o = M bridge $Index install; $o | Select-Object -Last 2 | ForEach-Object { Say "  install: $_" }
    if ($LASTEXITCODE -ne 0) { $problems += "modavd bridge install 退出码 $LASTEXITCODE" }
    M start $Index | Out-Null
    if ((Prop) -eq 'libmodavd_bridge.so') { $notes += 'modavd bridge install 又装上' } else { $problems += "再装后 native bridge=$(Prop)" }
} catch {
    $problems += "出错（第 $($_.InvocationInfo.ScriptLineNumber) 行）：$($_.Exception.Message)"
    Say $problems[-1]
} finally {
    if ($created) {
        M stop $Index | Select-Object -Last 1 | ForEach-Object { Say "  $_" }
        Remove-TestInstance $Index
    }
    Set-RootSdk $Root $old | Out-Null
}
Result ($problems.Count -eq 0) (($notes + $(if ($problems) { "问题：$($problems -join '；')" })) -join '；')
