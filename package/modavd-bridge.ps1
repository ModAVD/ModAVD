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
.SYNOPSIS
魔改 AVD：给一台实例装上 / 卸掉 arm64 执行桥（阴阳师等只有 arm64 版本、带加固的游戏要靠它）。

.DESCRIPTION
装进客体的是三个我们自己的文件，外加往 /vendor/build.prop 追加的一行。
Google 的翻译层 /system/lib64/libndk_translation.so 一个字节都不改。

  /system/lib64/libmodavd_bridge.so     native bridge 入口 shim（标签 u:object_r:system_lib_file:s0，644）
  /system/lib64/libmodavd_gate.so       执行桥伴生库（同上）
  /system/etc/init/modavd_bridge.rc     重新注册 arm64 binfmt_misc；remount 后补载 zram（标签 u:object_r:system_file:s0，644）
  /vendor/build.prop 追加一行           ro.dalvik.vm.native.bridge=libmodavd_bridge.so
  宿主 AVD 目录里的开机标记             modavd-writable-system（0.36.0 起的模拟器看到它就按可写系统镜像开机）

为什么是这个形状：
- 必须放 /system/lib64。libnativebridge 是在链接器的 "system" 命名空间里打开这个库的，
  那个命名空间的搜索路径不含 /vendor/lib64；而属性值里带 '/' 会被 NativeBridgeNameAcceptable() 拒掉，
  所以属性只能写裸 soname。
- 那个 .rc 不是可选项。镜像自带的 /system/etc/init/ndk_translation.rc 触发条件写的是字面值
  libndk_translation.so，属性一改，arm64 可执行文件就静默跑不起来了（app 加载的 JNI 库走结构体，不受影响）。
  我们这份 .rc 对新属性值重新注册同样的 handler。
- 属性只能改开机时读的 build.prop。setprop 改不动（ro. 只读）、/data/local.prop 跑在
  PropertyLoadBootDefaults() 之后、模拟器 -prop 同理。/vendor/build.prop 比 /system/build.prop 后加载、
  天然覆盖，而且 /vendor 没有 dm-verity。

规矩：
- 装、卸都要求这一次是按可写系统镜像开的机。不是的话 install 只在实例目录放开机标记、提示关机再照常开一次
  （0.36.0 起的模拟器看到标记就按可写系统镜像开），然后重跑；/system、/vendor 一概不动，退出码 3。
- 桥装在实例自己的 system / vendor 覆盖层里，只有按可写系统镜像开机才挂上。开机标记就是为了让装过桥的
  实例照常（AUTO-MAS）开机时也这样开；uninstall 最后会删掉它。
- 改 /vendor/build.prop 之前一定先备份（客体里 <文件>.modavdbak，宿主再留一份），
  写完先核对再谈重启：写坏一行 build.prop 就可能开不起来。
- 重启前先 adb shell sync（否则刚写的文件可能变成 0 字节，10-03 真出过这样的丢数据事故）。
- 幂等：三个文件的 SHA-256 和属性行都已经就位就什么都不做。可以每次开机照跑。
- 安装顺序是「先文件、最后属性」，卸载顺序是「先属性、再文件」。属性是唯一的开关，
  任何一步失败都不会留下「属性指向一个不存在的 shim」这种状态。

⚠ 退回旧版模拟器之前先卸掉

  装进去的东西写在**实例自己的** system.img.qcow2 / vendor.img.qcow2 覆盖层里，不在包里，更新器不碰 avd\。
  退回 0.35 及以前：旧模拟器不认开机标记，照常开机用的是原版镜像，桥不在（arm64 加固游戏照旧崩），但实例不会坏。
  真正危险的是按可写系统镜像开机、又只重置了一个覆盖层（例如删掉 system.img.qcow2 却留着 vendor.img.qcow2）：
  属性还指着 libmodavd_bridge.so、shim 没了 → **这台实例一个 arm64 应用都跑不了，而且没有任何提示**
  （x86_64 应用全正常）。shim 扛得住「伴生库不见了」，扛不住「shim 自己不见了」。

  所以：**降级前先 `modavd-bridge.ps1 uninstall -Reboot`**，确认 verify 回到
  ro.dalvik.vm.native.bridge=libndk_translation.so、开机标记已删，再换包。
  已经踩进去了也能救：把实例按可写系统镜像起来跑一次 uninstall；或者关掉模拟器，
  把该 AVD 目录下的 system.img.qcow2 和 vendor.img.qcow2 两个覆盖层**一起**删掉、并删掉 modavd-writable-system
  （userdata 和游戏数据不受影响，但 system / vendor 上的所有改动都会一起丢，包括客体 Vulkan 补丁）。

.PARAMETER Action
verify（默认，只读）、install、uninstall。

.PARAMETER Root
模拟器根目录，默认本脚本所在目录的上一级。

.PARAMETER Adb
adb.exe，默认 <Root>\sdk\platform-tools\adb.exe。

.PARAMETER Payload
三个文件所在的目录，默认 <Root>\sdk\emulator\resources\modavd。
升级时这个目录随 sdk\emulator 一起换，所以本脚本不写死任何哈希，一律现算现比。

.PARAMETER AdbPort
私有 adb server 端口。不给就用根目录 mas-avd.json 的 adbServerPort（AUTO-MAS / modavd 开的实例都登记在那里，
没写是 20050）；根目录没有 mas-avd.json 才用 20089。拒绝 5037。

.PARAMETER Serial
实例序列号，例如 emulator-20000。这个 server 上只有一台时可以不给；有多台时必须点名。

.PARAMETER Reboot
install / uninstall 做完后自己 sync + reboot，等开机完再跑一遍 verify。不给就只打印下一步的命令。

.PARAMETER BootTimeoutSec
-Reboot 时等开机的上限，默认 300 秒。

.PARAMETER GateTarget
开机后把 modavd.gate.target 设成这个值（逗号分隔的允许清单）。

这个属性**不持久**，每次开机都要重设；对设置之后才起的进程生效，所以设完要 am force-stop 再开游戏。

留空（默认）就不设 —— 这时伴生库走**编译进去的内置允许清单**，装完开箱即用，不需要任何东西在开机时设属性。
属性一旦设了就整体覆盖内置清单，所以 `-GateTarget none` 是关掉执行桥的办法（没有进程叫 none）。
清单的一条覆盖整个包：主进程、任意 `<包名>:<子进程>`、以及 app-zygote。

.PARAMETER Force
三个文件哈希一致也重装一遍。

.EXAMPLE
pwsh -NoProfile -File tools\modavd-bridge.ps1 verify -Serial emulator-20000

.EXAMPLE
# -WhatIf：所有读取照做（体检脚本会推到 /data/local/tmp 跑），所有对 /system、/vendor 和
# build.prop 的写入只打印不做。所以 -WhatIf 下「改属性之前再核对」那一步必然对不上，是正常的。
pwsh -NoProfile -File tools\modavd-bridge.ps1 install -Serial emulator-20000 -WhatIf

.EXAMPLE
pwsh -NoProfile -File tools\modavd-bridge.ps1 install -Serial emulator-20000 -Reboot

.EXAMPLE
pwsh -NoProfile -File tools\modavd-bridge.ps1 uninstall -Serial emulator-20000 -Reboot

.NOTES
退出码：0 完成（含「已安装且是当前版本」）；1 出错；2 用法或参数不对；
3 实例不是用可写系统镜像起的，装不了也卸不了；4 verify 没有全过。

约束（改本脚本时也要守住，和同目录另外三个脚本一致）：
- 必须能在 Windows PowerShell 5.1 上跑：不用 ??、三元、&&、-Parallel；文件存成 UTF-8 带 BOM。
- 不自己算哈希之外的事：不碰 5037，不写注册表，不需要管理员。
- 不用 emu kill；重启一律先 sync。
#>

[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Position = 0)]
    [ValidateSet('verify', 'install', 'uninstall')]
    [string]$Action = 'verify',
    [string]$Root,
    [string]$Adb,
    [string]$Payload,
    [int]$AdbPort = 20089,
    [string]$Serial,
    [switch]$Reboot,
    [ValidateRange(30, 1800)]
    [int]$BootTimeoutSec = 300,
    [string]$GateTarget = '',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
# 数字格式与系统区域无关（en-SE 等会把小数点写成逗号）
[Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::InvariantCulture

# ---------------------------------------------------------------- 常量

# 装进客体的三个文件：<载荷文件名> -> 目标路径 + SELinux 标签
$script:Items = @(
    [pscustomobject]@{ Key = 'shim'; Name = 'libmodavd_bridge.so'; Target = '/system/lib64/libmodavd_bridge.so'; Context = 'u:object_r:system_lib_file:s0' }
    [pscustomobject]@{ Key = 'gate'; Name = 'libmodavd_gate.so'; Target = '/system/lib64/libmodavd_gate.so'; Context = 'u:object_r:system_lib_file:s0' }
    [pscustomobject]@{ Key = 'rc'; Name = 'modavd_bridge.rc'; Target = '/system/etc/init/modavd_bridge.rc'; Context = 'u:object_r:system_file:s0' }
)
$script:PropLine = 'ro.dalvik.vm.native.bridge=libmodavd_bridge.so'
$script:StockBridge = 'libndk_translation.so'
$script:GuestDir = '/data/local/tmp/modavd-bridge'
$script:AdbExe = ''
$script:Dev = @()
$script:Failed = $false

# ---------------------------------------------------------------- 基础

function Say([string]$Text) { Write-Host $Text }
function Head([string]$Text) { Write-Host ''; Write-Host "== $Text" }
function Note([string]$Text) { Write-Host "   $Text" }
function Warn([string]$Text) { Write-Host "   ! $Text" -ForegroundColor Yellow }
function Bad([string]$Text) { Write-Host "   x $Text" -ForegroundColor Red }
function Good([string]$Text) { Write-Host "   + $Text" -ForegroundColor Green }

function Die([string]$Text, [int]$Code) {
    Write-Host ''
    Write-Host $Text -ForegroundColor Red
    exit $Code
}

# 不用 Get-FileHash：经 cmd 调起的 Windows PowerShell 5.1 有时找不到它（同 fetch-components.ps1）
function Sha256File([string]$Path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try { $bytes = $sha.ComputeHash($stream) } finally { $stream.Dispose(); $sha.Dispose() }
    return (-join ($bytes | ForEach-Object { $_.ToString('x2') }))
}

# 跑 adb，返回退出码、stdout、stderr。stderr 走文件：PS 5.1 下把原生命令的 stderr
# 用 2>&1 并进成功流，在 $ErrorActionPreference='Stop' 时会直接抛异常。
function Invoke-Adb {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)
    # 跑 adb 和抓它的 stderr 都是宿主侧的脚手架，不是被 -WhatIf 预览的那个操作：
    # 真正会改客体的每一步都各自过 ShouldProcess。重定向运算符本身也认 $WhatIfPreference，
    # 不在这里关掉的话 -WhatIf 下连 stderr 文件都不会建。函数作用域，不外泄。
    $WhatIfPreference = $false
    # Windows PowerShell 5.1 在 ErrorActionPreference=Stop 下，会把原生命令写到 stderr 的每一行
    # （adb push 的「1 file pushed」就写在 stderr）当成终止错误抛出，哪怕已经重定向到文件；
    # PowerShell 7.2 起不这样。函数作用域里放成 Continue，stderr 照样进文件，由调用方看退出码。
    $ErrorActionPreference = 'Continue'
    $errFile = Join-Path $script:WorkDir ('adb-err-' + [guid]::NewGuid().ToString('N') + '.txt')
    $code = -1
    $out = ''
    $err = ''
    try {
        $raw = & $script:AdbExe @Arguments 2>$errFile
        $code = $LASTEXITCODE
        if ($null -ne $raw) { $out = ($raw | Out-String) }
        if (Test-Path -LiteralPath $errFile) { $err = [IO.File]::ReadAllText($errFile) }
        # 5.1 把 stderr 写成格式化的错误记录（「adb.exe : <原文>」加 CategoryInfo 等行），还原成原文
        $err = (($err -split "`r?`n") | Where-Object { $_.Trim() -and $_ -notmatch '^\s*\+ (CategoryInfo|FullyQualifiedErrorId)\s*:' -and $_ -notmatch '^\s*\+\s*$' } | ForEach-Object { $_ -replace '^\S+\.exe : ', '' }) -join "`n"
    } finally {
        if (Test-Path -LiteralPath $errFile) { Remove-Item -LiteralPath $errFile -Force -ErrorAction SilentlyContinue }
    }
    return [pscustomobject]@{ Code = $code; Out = $out.Trim(); Err = $err.Trim() }
}

function Invoke-Device { param([Parameter(Mandatory = $true)][string[]]$Arguments) return (Invoke-Adb ($script:Dev + $Arguments)) }
function Invoke-Guest { param([Parameter(Mandatory = $true)][string]$Command) return (Invoke-Device @('shell', $Command)) }

# 客体脚本：生成到宿主工作目录（一律 LF），push 过去，用 su 0 sh 跑。
# 带引号、重定向、循环的东西一律走这条路，不去猜 PowerShell -> adb -> sh 三层引号。
function Invoke-GuestScript {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Body
    )
    $local = Join-Path $script:WorkDir $Name
    [IO.File]::WriteAllText($local, ($Body -replace "`r", ''), (New-Object Text.UTF8Encoding($false)))
    $remote = "$script:GuestDir/$Name"
    $push = Invoke-Device @('push', $local, $remote)
    if ($push.Code -ne 0) { throw "push $Name 失败：$($push.Err)$($push.Out)" }
    return (Invoke-Guest "su 0 sh $remote")
}

# 把 KEY=VALUE 的客体报告读成哈希表
function Parse-Report([string]$Text) {
    $map = @{}
    foreach ($line in ($Text -split "`n")) {
        $t = $line.Trim()
        $i = $t.IndexOf('=')
        if ($i -gt 0) { $map[$t.Substring(0, $i)] = $t.Substring($i + 1) }
    }
    return $map
}

function Pv([hashtable]$Map, [string]$Key) {
    if ($Map.ContainsKey($Key)) { return [string]$Map[$Key] }
    return ''
}

function Check([string]$What, [bool]$Ok, [string]$Detail) {
    if ($Ok) { Good "$What  $Detail" } else { Bad "$What  $Detail"; $script:Failed = $true }
}

# ---------------------------------------------------------------- 客体脚本

$script:ProbeSh = @'
# ModAVD arm64 执行桥：只读体检。由 tools\modavd-bridge.ps1 生成。
e() { printf '%s=%s\n' "$1" "$2"; }
q() {
  if [ -e "$2" ]; then
    e "$1_sha" "$(sha256sum "$2" 2>/dev/null | cut -d' ' -f1)"
    e "$1_ctx" "$(ls -Z "$2" 2>/dev/null | cut -d' ' -f1)"
    e "$1_mode" "$(stat -c %a "$2" 2>/dev/null)"
    e "$1_size" "$(stat -c %s "$2" 2>/dev/null)"
  else
    e "$1_sha" ""
  fi
}
e sdk "$(getprop ro.build.version.sdk)"
e release "$(getprop ro.build.version.release)"
e fingerprint "$(getprop ro.build.fingerprint)"
e abilist "$(getprop ro.product.cpu.abilist)"
e isa_arm64 "$(getprop ro.dalvik.vm.isa.arm64)"
e isa_arm "$(getprop ro.dalvik.vm.isa.arm)"
e exec_enabled "$(getprop ro.enable.native.bridge.exec)"
e prop_running "$(getprop ro.dalvik.vm.native.bridge)"
e berberis "$(getprop ro.berberis.version)"
e gate_target "$(getprop modavd.gate.target)"
e boot_completed "$(getprop sys.boot_completed)"
e uid "$(id -u)"
q shim /system/lib64/libmodavd_bridge.so
q gate /system/lib64/libmodavd_gate.so
q rc /system/etc/init/modavd_bridge.rc
q real /system/lib64/libndk_translation.so
vp="$(readlink -f /vendor/build.prop 2>/dev/null)"
e vp "$vp"
if [ -n "$vp" ] && [ -f "$vp" ]; then
  e vp_size "$(stat -c %s "$vp")"
  e vp_sha "$(sha256sum "$vp" | cut -d' ' -f1)"
  e vp_lines "$(wc -l < "$vp" | tr -d ' ')"
  e vp_ours "$(grep -c '^ro\.dalvik\.vm\.native\.bridge=libmodavd_bridge\.so$' "$vp")"
  e vp_any "$(grep -c '^ro\.dalvik\.vm\.native\.bridge=' "$vp")"
  e vp_last "$(tail -n 1 "$vp")"
  if [ -f "$vp.modavdbak" ]; then e vp_bak 1; else e vp_bak 0; fi
else
  e vp_size ""; e vp_sha ""; e vp_lines ""; e vp_ours 0; e vp_any 0; e vp_last ""; e vp_bak 0
fi
for n in arm64_exe arm64_dyn arm_exe arm_dyn; do
  p=/proc/sys/fs/binfmt_misc/$n
  if [ -e "$p" ]; then e "binfmt_$n" "$(head -n 1 "$p" 2>/dev/null)"; else e "binfmt_$n" "(absent)"; fi
done
e mounts "$(grep -E ' / | /system | /vendor ' /proc/mounts 2>/dev/null | tr '\n' ';')"
e zram_swap "$(grep -c '^/dev/block/zram0 ' /proc/swaps 2>/dev/null)"
e stale"$(ls -d /system/lib64/*.modavd-new /system/etc/init/*.modavd-new 2>/dev/null | tr '\n' ';')"
'@

$script:WriteTestSh = @'
# ModAVD：实例到底是不是用可写系统镜像起的。只在 /system、/vendor 里建一个空探针文件再删掉。
t() {
  f="$1/.modavd-writetest"
  if ( : > "$f" ) 2>/dev/null; then printf '%s=1\n' "$2"; rm -f "$f"; else printf '%s=0\n' "$2"; fi
}
mkdir -p __GUESTDIR__ 2>/dev/null
t /system/lib64 w_system_lib64
t /system/etc/init w_system_etc_init
vp="$(readlink -f /vendor/build.prop 2>/dev/null)"
if [ -n "$vp" ]; then t "$(dirname "$vp")" w_vendor; else printf 'w_vendor=0\n'; fi
printf 'uid=%s\n' "$(id -u)"
'@

# 追加属性行。先备份，再补齐末尾换行，再追加，再逐字节核对；核对不过就立刻还原。
$script:PropAddSh = @'
line='ro.dalvik.vm.native.bridge=libmodavd_bridge.so'
vp="$(readlink -f /vendor/build.prop 2>/dev/null)"
if [ -z "$vp" ] || [ ! -f "$vp" ]; then echo "FAIL: /vendor/build.prop not found"; exit 1; fi
bak="$vp.modavdbak"
# 备份只做一次：已有的那份是 stock 参照，绝不能被改过的内容盖掉
if [ ! -f "$bak" ]; then
  cp -a "$vp" "$bak" || { echo "FAIL: backup failed"; exit 1; }
  if [ "$(sha256sum "$vp" | cut -d' ' -f1)" != "$(sha256sum "$bak" | cut -d' ' -f1)" ]; then
    echo "FAIL: backup differs from $vp"; exit 1
  fi
  echo "INFO: backed up to $bak"
else
  echo "INFO: keeping the existing backup $bak"
fi
if grep -q "^$line\$" "$vp"; then echo "OK: already present"; grep -n 'native.bridge' "$vp"; exit 0; fi
# build.prop 末尾没有换行的话，我们这行会和上一条属性粘在一起 —— 先补上，再记基准
if [ -n "$(tail -c 1 "$vp")" ]; then printf '\n' >> "$vp"; echo "INFO: added the missing final newline"; fi
bsize="$(stat -c %s "$vp")"
bsha="$(head -c "$bsize" "$vp" | sha256sum | cut -d' ' -f1)"
blines="$(wc -l < "$vp" | tr -d ' ')"
printf '%s\n' "$line" >> "$vp"
# 重启之前先核对：前缀逐字节没变，尾巴正好是我们那一行
asize="$(stat -c %s "$vp")"
alines="$(wc -l < "$vp" | tr -d ' ')"
bad=0
if [ ! -s "$vp" ]; then echo "FAIL: $vp is empty"; bad=1; fi
if [ "$(head -c "$bsize" "$vp" | sha256sum | cut -d' ' -f1)" != "$bsha" ]; then echo "FAIL: the first $bsize bytes changed"; bad=1; fi
if [ "$(tail -c "$((asize - bsize))" "$vp")" != "$line" ]; then echo "FAIL: the appended bytes are not exactly our line"; bad=1; fi
if [ "$(tail -n 1 "$vp")" != "$line" ]; then echo "FAIL: last line is not ours"; bad=1; fi
if [ "$alines" != "$((blines + 1))" ]; then echo "FAIL: line count $blines -> $alines"; bad=1; fi
if [ "$(grep -c "^$line\$" "$vp")" != "1" ]; then echo "FAIL: our line appears more than once"; bad=1; fi
if [ "$bad" != 0 ]; then
  if cp -a "$bak" "$vp"; then sync; echo "INFO: restored $vp from $bak - nothing changed, do not reboot yet"
  else echo "FAIL: COULD NOT RESTORE $vp FROM $bak - do not reboot, fix it by hand"; fi
  exit 1
fi
sync
echo "OK: appended, lines $blines -> $alines, bytes $bsize -> $asize"
echo "TAIL3:"
tail -n 3 "$vp"
'@

# 删掉属性行。先在 /data 下算好结果并校验，再原地覆盖写（保住 inode、权限和 SELinux 标签）。
$script:PropDelSh = @'
line='ro.dalvik.vm.native.bridge=libmodavd_bridge.so'
vp="$(readlink -f /vendor/build.prop 2>/dev/null)"
if [ -z "$vp" ] || [ ! -f "$vp" ]; then echo "FAIL: /vendor/build.prop not found"; exit 1; fi
bak="$vp.modavdbak"
n="$(grep -c "^$line\$" "$vp")"
if [ "$n" = "0" ]; then echo "OK: not present"; grep -n 'native.bridge' "$vp"; exit 0; fi
mkdir -p __GUESTDIR__ 2>/dev/null
tmp=__GUESTDIR__/build.prop.new
blines="$(wc -l < "$vp" | tr -d ' ')"
grep -v "^$line\$" "$vp" > "$tmp" || { echo "FAIL: could not write $tmp"; exit 1; }
alines="$(wc -l < "$tmp" | tr -d ' ')"
bad=0
if [ ! -s "$tmp" ]; then echo "FAIL: the filtered file is empty"; bad=1; fi
if [ "$alines" != "$((blines - n))" ]; then echo "FAIL: line count $blines -> $alines, expected $((blines - n))"; bad=1; fi
if [ "$(grep -c 'libmodavd_bridge' "$tmp")" != "0" ]; then echo "FAIL: libmodavd_bridge still mentioned"; bad=1; fi
if [ "$bad" != 0 ]; then echo "FAIL: $vp left untouched"; exit 1; fi
# 原地覆盖写，不用 mv：/vendor/etc 里新建的文件会拿到目录默认标签，而我们要保住原文件的标签
if ! cat "$tmp" > "$vp"; then echo "FAIL: write to $vp failed - restore from $bak by hand"; exit 1; fi
sync
if [ "$(sha256sum "$vp" | cut -d' ' -f1)" != "$(sha256sum "$tmp" | cut -d' ' -f1)" ]; then
  echo "FAIL: $vp differs from the checked content, restoring from $bak"
  cat "$bak" > "$vp"; sync; exit 1
fi
echo "OK: removed $n line(s), lines $blines -> $alines"
echo "GREP:"
grep -n 'native.bridge' "$vp"
if [ -f "$bak" ]; then
  if [ "$(sha256sum "$vp" | cut -d' ' -f1)" = "$(sha256sum "$bak" | cut -d' ' -f1)" ]; then
    echo "INFO: $vp is now byte-identical to the stock backup $bak"
  else
    echo "INFO: $vp differs from $bak (something else changed it since the install)"
  fi
fi
'@

# ---------------------------------------------------------------- 准备

if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }
$Root = [IO.Path]::GetFullPath($Root)

# 不给 -AdbPort：实例登记在根目录 mas-avd.json 的 adbServerPort 那个 server 上（AUTO-MAS / modavd 开机都用它，
# 没写就是 AUTO-MAS 的默认 20050）。另起一个 server 看不到这些实例。没有 mas-avd.json（不是 AUTO-MAS 加过的根）才用 20089。
$script:AdbPortFrom = '-AdbPort'
if (-not $PSBoundParameters.ContainsKey('AdbPort')) {
    $script:AdbPortFrom = '默认值'
    $meta = Join-Path $Root 'mas-avd.json'
    if (Test-Path -LiteralPath $meta) {
        $AdbPort = 20050
        $script:AdbPortFrom = 'mas-avd.json 没写，AUTO-MAS 的默认值'
        try {
            $p = (Get-Content -LiteralPath $meta -Raw -Encoding UTF8 | ConvertFrom-Json).adbServerPort
            if ($p -is [int] -or $p -is [long]) { $AdbPort = [int]$p; $script:AdbPortFrom = 'mas-avd.json 的 adbServerPort' }
        } catch { Warn "读不了 $meta（$($_.Exception.Message)），按默认 20050" }
    }
}
if ($AdbPort -eq 5037) { Die '拒绝 adb 的默认端口 5037（-AdbPort）。魔改 AVD 一律用自己的私有 server。' 2 }
if ($AdbPort -lt 1 -or $AdbPort -gt 65535) { Die "-AdbPort 不是合法端口：$AdbPort" 2 }

# 提示「重跑」时照用户这一次实际给的参数拼命令：改过端口、路径的人照抄也不会跑错
$script:BoundArgs = @{}
foreach ($k in $PSBoundParameters.Keys) { $script:BoundArgs[$k] = $PSBoundParameters[$k] }
function Get-RerunCommand([string]$Act, [switch]$WithReboot) {
    $exe = if ($PSVersionTable.PSEdition -eq 'Core') { 'pwsh -NoProfile' } else { 'powershell -NoProfile -ExecutionPolicy Bypass' }
    $parts = @("$exe -File `"$PSCommandPath`" $Act")
    foreach ($k in @('Root', 'Adb', 'Payload', 'AdbPort', 'BootTimeoutSec', 'GateTarget')) {
        if ($script:BoundArgs.ContainsKey($k)) {
            $v = [string]$script:BoundArgs[$k]
            if ($v -match '\s' -or $v -eq '') { $v = "`"$v`"" }
            $parts += "-$k $v"
        }
    }
    $parts += "-Serial $Serial"
    if ($WithReboot) { $parts += '-Reboot' }
    return ($parts -join ' ')
}

if (-not $Adb) { $Adb = Join-Path $Root 'sdk\platform-tools\adb.exe' }
if (-not $Payload) { $Payload = Join-Path $Root 'sdk\emulator\resources\modavd' }

if (-not (Test-Path -LiteralPath $Adb)) {
    Die "找不到 adb：$Adb`n包里的 adb 要先双击「补全组件.bat」下载；也可以用 -Adb 指一个。" 2
}
$script:AdbExe = (Resolve-Path -LiteralPath $Adb).Path

# 工作目录：生成的客体脚本、build.prop 的宿主备份都放这里。更新器不碰 logs\。
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$script:WorkDir = Join-Path $Root "logs\modavd-bridge\$stamp"
# -WhatIf:$false：这个目录是脚手架（生成的客体脚本、备份、日志），-WhatIf 时也要真的建出来
try { New-Item -ItemType Directory -Path $script:WorkDir -Force -WhatIf:$false | Out-Null }
catch { $script:WorkDir = Join-Path $env:TEMP "modavd-bridge-$stamp"; New-Item -ItemType Directory -Path $script:WorkDir -Force -WhatIf:$false | Out-Null }

# 载荷：不写死任何哈希。升级时这三个文件随 sdk\emulator 一起换，现算现比才不会和包版本耦合。
$payloadFiles = @{}
$missing = @()
foreach ($item in $script:Items) {
    $p = Join-Path $Payload $item.Name
    if (Test-Path -LiteralPath $p) { $payloadFiles[$item.Key] = (Resolve-Path -LiteralPath $p).Path } else { $missing += $item.Name }
}
if ($Action -ne 'uninstall' -and $missing.Count -gt 0) {
    Die ("载荷不全，缺：{0}`n找的目录是 {1}`n（-Payload 可以改；升级时这个目录随 sdk\emulator 一起换）" -f ($missing -join '、'), $Payload) 2
}
$hostSha = @{}
foreach ($k in $payloadFiles.Keys) { $hostSha[$k] = Sha256File $payloadFiles[$k] }

Say "ModAVD arm64 执行桥 — $Action"
Note "根目录     $Root"
Note "adb        $script:AdbExe  (-P $AdbPort，$script:AdbPortFrom)"
Note "载荷       $Payload"
foreach ($item in $script:Items) {
    if ($payloadFiles.ContainsKey($item.Key)) {
        Note ("  {0,-22} {1}  {2} 字节" -f $item.Name, $hostSha[$item.Key].Substring(0, 16), (Get-Item -LiteralPath $payloadFiles[$item.Key]).Length)
    } else {
        Note ("  {0,-22} 不在载荷里（卸载不需要）" -f $item.Name)
    }
}
Note "工作目录   $script:WorkDir"

# ---------------------------------------------------------------- 选实例

$devices = Invoke-Adb @('-P', "$AdbPort", 'devices')
if ($devices.Code -ne 0) {
    Die ("adb -P $AdbPort devices 失败（{0}）：{1}{2}`n模拟器开着吗？端口对吗？" -f $devices.Code, $devices.Err, $devices.Out) 1
}
$online = @()
foreach ($line in ($devices.Out -split "`n")) {
    if ($line -match '^(\S+)\s+device\s*$') { $online += $Matches[1] }
}
if (-not $Serial) {
    if ($online.Count -eq 1) { $Serial = $online[0] }
    elseif ($online.Count -eq 0) { Die "adb server $AdbPort 上没有在线的实例。先把实例开起来（AUTO-MAS、ModAVD.exe 或 tools\modavd.exe start 都行）；端口不对就用 -AdbPort 指定。" 1 }
    else { Die ("adb server $AdbPort 上有 {0} 台实例：{1}`n请用 -Serial 点名一台。" -f $online.Count, ($online -join '、')) 2 }
} elseif ($online -notcontains $Serial) {
    Die ("adb server $AdbPort 上没有 {0}（在线的是：{1}）" -f $Serial, ($online -join '、')) 2
}
$script:Dev = @('-P', "$AdbPort", '-s', $Serial)
Note "实例       $Serial"

# ---------------------------------------------------------------- 实例目录与开机标记
# 桥装在实例自己的 system / vendor 覆盖层里，这两个覆盖层只有按可写系统镜像开机才挂上；照常开机
# （AUTO-MAS 不带 -writable-system）时用的是原版镜像，桥就不在了（10-29 实测）。0.36.0 起的模拟器
# 看到 AVD 目录里的这个标记，就自动按可写系统镜像开这台实例，所以装完以后照常开机桥也在。
$script:MarkerName = 'modavd-writable-system'
$script:AvdDir = $null
$script:Marker = $null
$ap = Invoke-Device @('emu', 'avd', 'path')
foreach ($l in ($ap.Out -split "`n")) {
    $t = $l.Trim()
    if ($t -and $t -ne 'OK' -and (Test-Path -LiteralPath $t -PathType Container)) { $script:AvdDir = (Resolve-Path -LiteralPath $t).Path; break }
}
if ($script:AvdDir) {
    $script:Marker = Join-Path $script:AvdDir $script:MarkerName
    Note "实例目录   $script:AvdDir"
} else {
    Warn '问不出这台实例的 AVD 目录（adb emu avd path），没法放 / 删开机标记。'
}
$script:EmuBuild = 0
$sprops = Join-Path $Root 'sdk\emulator\source.properties'
if (Test-Path -LiteralPath $sprops) {
    $bm = Select-String -LiteralPath $sprops -Pattern '^Pkg\.BuildId=mas-(\d+)'
    if ($bm) { $script:EmuBuild = [int]$bm.Matches[0].Groups[1].Value }
}

function Set-WritableMarker {
    if (-not $script:Marker) { Warn '没放开机标记（不知道实例目录）：照常开机时桥会消失。'; return $false }
    if (Test-Path -LiteralPath $script:Marker) { Good "开机标记已在：$script:Marker"; return $true }
    if (-not $PSCmdlet.ShouldProcess($script:Marker, '写开机标记')) { return $false }
    $text = "ModAVD arm64 执行桥：这台实例装了桥（或正要装）。`r`n" +
            "0.36.0 起的模拟器看到这个文件，就按可写系统镜像开机，桥装在实例自己的 system / vendor 覆盖层里，只有这样才挂得上。`r`n" +
            "不要手动删；要卸桥请跑 tools\modavd-bridge.ps1 uninstall，它会连同这个文件一起删掉。`r`n"
    [IO.File]::WriteAllText($script:Marker, $text, (New-Object Text.UTF8Encoding($true)))
    Good "放了开机标记 $script:Marker"
    if ($script:EmuBuild -gt 0 -and $script:EmuBuild -lt 36) {
        Warn "这个根目录的模拟器是 mas-$script:EmuBuild，不认这个标记（0.36.0 起才认）：照常开机时桥不在，要升级模拟器。"
    }
    return $true
}

# ---------------------------------------------------------------- GL 传输
# 装桥的实例跑的是只有 arm64 版本的游戏。阴阳师渲染线程的时间大头，花在把 GL 命令一条条经 goldfish 管道写进内核
# （pipe 传输）。asg 用共享内存环形缓冲：10-30 在 0.36.0 上实测，pipe 44–60 fps、平均帧时间 20–22 ms，
# asg 58.5–60.7 fps、12.5–13.1 ms（同机还开着别的模拟器）。只改这台实例的 config.ini（hw.gltransport=asg），
# 下次冷启动生效；别的实例不动。我们设的在开机标记里记一笔，uninstall 只撤我们设的。
$script:GlNote = 'gltransport=asg'
function Get-AvdConfig { if ($script:AvdDir) { return (Join-Path $script:AvdDir 'config.ini') }; return $null }

function Set-GlTransportAsg {
    $cfg = Get-AvdConfig
    if (-not $cfg -or -not (Test-Path -LiteralPath $cfg)) { Warn '找不到这台实例的 config.ini，GL 传输没改（不影响桥）。'; return }
    $text = [IO.File]::ReadAllText($cfg)
    $m = [regex]::Match($text, '(?m)^hw\.gltransport\s*=\s*(\S*)\s*$')
    if ($m.Success) {
        if ($m.Groups[1].Value -eq 'asg') { Good 'GL 传输已经是 asg'; return }
        Warn ("这台实例的 hw.gltransport 是 {0}，不是我们设的，不改。" -f $m.Groups[1].Value)
        return
    }
    if (-not $PSCmdlet.ShouldProcess($cfg, '加 hw.gltransport=asg')) { return }
    $nl = "`n"; if ($text.Contains("`r`n")) { $nl = "`r`n" }
    [IO.File]::WriteAllText($cfg, $text.TrimEnd("`r", "`n") + $nl + 'hw.gltransport=asg' + $nl, (New-Object Text.UTF8Encoding($false)))
    if ($script:Marker -and (Test-Path -LiteralPath $script:Marker)) {
        [IO.File]::AppendAllText($script:Marker, "$script:GlNote（modavd-bridge 设的，uninstall 会撤掉）`r`n", (New-Object Text.UTF8Encoding($false)))
    }
    Good 'GL 传输设成 asg（config.ini），下次冷启动生效'
}

function Restore-GlTransport {
    if (-not $script:Marker -or -not (Test-Path -LiteralPath $script:Marker)) { return }
    if (-not (Select-String -LiteralPath $script:Marker -SimpleMatch $script:GlNote -Quiet)) { return }
    $cfg = Get-AvdConfig
    if (-not $cfg -or -not (Test-Path -LiteralPath $cfg)) { return }
    if (-not $PSCmdlet.ShouldProcess($cfg, '撤掉 hw.gltransport=asg')) { return }
    $text = [IO.File]::ReadAllText($cfg)
    [IO.File]::WriteAllText($cfg, [regex]::Replace($text, '(?m)^hw\.gltransport=asg\r?\n?', ''), (New-Object Text.UTF8Encoding($false)))
    Good '撤掉了装桥时设的 hw.gltransport=asg，下次冷启动回到默认传输'
}

function Remove-WritableMarker {
    if (-not $script:Marker) { Warn '不知道实例目录，没删开机标记；它要是还在，这台实例会一直按可写系统镜像开机（无害）。'; return }
    if (-not (Test-Path -LiteralPath $script:Marker)) { Good '没有开机标记'; return }
    if (-not $PSCmdlet.ShouldProcess($script:Marker, '删开机标记')) { return }
    Remove-Item -LiteralPath $script:Marker -Force
    Good "删掉开机标记 $script:Marker（以后照常开机就是原版 system 镜像）"
}

# ---------------------------------------------------------------- 体检

function Get-Probe {
    $r = Invoke-Guest "mkdir -p $script:GuestDir"
    if ($r.Code -ne 0) { throw "客体里建不了 $script:GuestDir：$($r.Err)$($r.Out)" }
    $res = Invoke-GuestScript 'probe.sh' $script:ProbeSh
    if ($res.Code -ne 0) { throw "客体体检失败：$($res.Err)$($res.Out)" }
    return (Parse-Report $res.Out)
}

$probe = @{}
try { $probe = Get-Probe }
catch { Die ("读不到实例情况：{0}`n模拟器刚开机还没起完 adbd，或者这台实例不是我们的实例。" -f $_.Exception.Message) 1 }
Head '实例情况'
Note ("Android {0}（API {1}）  abilist={2}" -f (Pv $probe 'release'), (Pv $probe 'sdk'), (Pv $probe 'abilist'))
Note ("翻译层 /system/lib64/libndk_translation.so  {0}" -f $(if ((Pv $probe 'real_sha')) { (Pv $probe 'real_sha').Substring(0, 16) + "  berberis=" + (Pv $probe 'berberis') } else { '不存在（这个镜像没有 arm 转译层，桥没有意义）' }))
Note ("ro.dalvik.vm.native.bridge = {0}（运行中的值）" -f $(if ((Pv $probe 'prop_running')) { Pv $probe 'prop_running' } else { '(空)' }))
Note ("/vendor/build.prop -> {0}  共 {1} 行；我们那行 {2} 条，native.bridge 共 {3} 条；备份 {4}" -f `
    (Pv $probe 'vp'), (Pv $probe 'vp_lines'), (Pv $probe 'vp_ours'), (Pv $probe 'vp_any'), $(if ((Pv $probe 'vp_bak') -eq '1') { '有' } else { '没有' }))
foreach ($item in $script:Items) {
    $gs = Pv $probe ($item.Key + '_sha')
    if ($gs) {
        $same = $payloadFiles.ContainsKey($item.Key) -and $gs -eq $hostSha[$item.Key]
        Note ("{0,-22} 已装 {1}  {2}  {3}  {4}" -f $item.Name, $gs.Substring(0, 16), (Pv $probe ($item.Key + '_mode')), (Pv $probe ($item.Key + '_ctx')), $(if ($same) { '= 载荷' } else { '≠ 载荷' }))
    } else {
        Note ("{0,-22} 没装" -f $item.Name)
    }
}
if ((Pv $probe 'stale')) { Warn ("客体里有上次没做完留下的临时文件：" + (Pv $probe 'stale')) }

$filesCurrent = $true
foreach ($item in $script:Items) {
    if (-not $payloadFiles.ContainsKey($item.Key)) { $filesCurrent = $false; continue }
    if ((Pv $probe ($item.Key + '_sha')) -ne $hostSha[$item.Key]) { $filesCurrent = $false }
    if ((Pv $probe ($item.Key + '_mode')) -ne '644') { $filesCurrent = $false }
    if ((Pv $probe ($item.Key + '_ctx')) -ne $item.Context) { $filesCurrent = $false }
}
$propInstalled = ((Pv $probe 'vp_ours') -eq '1')
$propRunning = ((Pv $probe 'prop_running') -eq 'libmodavd_bridge.so')
$shimPresent = [bool](Pv $probe 'shim_sha')

# 最危险的状态：属性指着我们，shim 不在 —— 这台实例现在一个 arm64 应用都跑不了
if (($propRunning -or $propInstalled) -and -not $shimPresent) {
    Warn '属性指向 libmodavd_bridge.so，但 /system/lib64 里没有这个文件。'
    Warn '这台实例现在没有任何 arm64 支持（x86_64 应用不受影响，所以界面上看不出来）。'
    if ($Action -eq 'verify') { Warn '跑一次 install 修回来，或者跑 uninstall 回到 stock。' }
}

# ---------------------------------------------------------------- verify

function Invoke-Verify([hashtable]$P, [bool]$ExpectInstalled) {
    $script:Failed = $false
    Head $(if ($ExpectInstalled) { '核对：桥应该在工作' } else { '核对：应该回到 stock' })
    $running = Pv $P 'prop_running'
    if ($ExpectInstalled) {
        Check 'ro.dalvik.vm.native.bridge' ($running -eq 'libmodavd_bridge.so') "= $running"
    } else {
        Check 'ro.dalvik.vm.native.bridge' ($running -eq $script:StockBridge) "= $running（期望 $script:StockBridge）"
    }
    $exe = Pv $P 'binfmt_arm64_exe'
    $dyn = Pv $P 'binfmt_arm64_dyn'
    Check 'binfmt_misc/arm64_exe' ($exe -like 'enabled*') "= $exe"
    Check 'binfmt_misc/arm64_dyn' ($dyn -like 'enabled*') "= $dyn"
    Check 'abilist 里还有 arm64' ((Pv $P 'abilist') -like '*arm64-v8a*') ("= " + (Pv $P 'abilist'))
    foreach ($item in $script:Items) {
        $gs = Pv $P ($item.Key + '_sha')
        if ($ExpectInstalled) {
            $want = ''
            if ($hostSha.ContainsKey($item.Key)) { $want = $hostSha[$item.Key] }
            $ok = ($gs -eq $want) -and ((Pv $P ($item.Key + '_mode')) -eq '644') -and ((Pv $P ($item.Key + '_ctx')) -eq $item.Context)
            Check $item.Name $ok ("{0} {1} {2}" -f $(if ($gs) { $gs.Substring(0, 16) } else { '没装' }), (Pv $P ($item.Key + '_mode')), (Pv $P ($item.Key + '_ctx')))
        } else {
            Check ($item.Name + ' 不在客体里') (-not $gs) $(if ($gs) { '还在 ' + $item.Target } else { '对的' })
        }
    }
    if ($ExpectInstalled) {
        Check '/vendor/build.prop 有且只有一行我们的属性' ((Pv $P 'vp_ours') -eq '1') ("vp_ours=" + (Pv $P 'vp_ours'))
        # remount 之后开机早期那次 modprobe zram 常被拒（见 modavd_bridge.rc），rc 等 dlkm_loader 走完再补一次。
        # swapon_all 在开机完成时才跑，所以只在开机完成以后提醒，不算失败。
        if ((Pv $P 'boot_completed') -eq '1' -and (Pv $P 'zram_swap') -ne '1') {
            Warn ("没有 zram 交换区（zram_swap={0}）：客体里的 modavd_bridge.rc 是旧版的话，重跑 install 换上新的再冷启动一次" -f (Pv $P 'zram_swap'))
        }
    } else {
        Check '/vendor/build.prop 里没有我们的属性' ((Pv $P 'vp_ours') -eq '0') ("vp_ours=" + (Pv $P 'vp_ours'))
    }

    # shim 自己说的话。logcat 里不止一次构造（每个 app 进程一次），只要没有 FAILED / INERT 就算过。
    $log = Invoke-Device @('logcat', '-b', 'all', '-d', '-s', 'ModAvdNB:V')
    $text = $log.Out
    $nbLog = Join-Path $script:WorkDir "logcat-ModAvdNB-$Serial.txt"
    [IO.File]::WriteAllText($nbLog, $text, (New-Object Text.UTF8Encoding($false)))
    if ($ExpectInstalled) {
        if ($text -match 'shim-ctor OK version=(\d+) wrapped=') {
            Check 'ModAvdNB: shim-ctor OK' $true "version=$($Matches[1])"
        } else {
            # 这一行只在开机时打；实例开久了日志会滚掉。退而看 zygote64 有没有映射 shim：
            # libnativebridge 在 zygote 里就把它 dlopen 了，shim 没装上（INERT 被拒）就不会留在映射里。
            $z = Invoke-GuestScript 'zygmap.sh' 'p=$(pidof zygote64); grep -c libmodavd_bridge.so /proc/$p/maps'
            $mapped = ($z.Out.Trim() -match '^[1-9]')
            Check 'shim 已加载（zygote64 映射里有 libmodavd_bridge.so）' $mapped $(if ($mapped) { '开机日志已滚掉，按 zygote64 的映射判断' } else { "日志里没有 shim-ctor OK，zygote64 里也没映射（$($z.Out.Trim())）" })
        }
        if ($text -match 'shim-ctor (FAILED|INERT)') { Bad "ModAvdNB 里有 shim-ctor $($Matches[1])：翻译层没打开成，这台实例没有 arm64"; $script:Failed = $true }
        if ($text -match 'shim-ctor companion \S+ loaded handle=') { Good 'ModAvdNB: 伴生库已加载' }
        elseif ($text -match 'shim-ctor companion dlopen\(([^)]*)\) failed') { Warn "伴生库没加载上（$($Matches[1])）：桥的入口在，执行桥不工作。游戏会照旧崩。" }
        elseif ($text -match 'shim-ctor companion disabled') { Warn '伴生库被 modavd.nb.companion 关掉了' }
        else { Warn 'ModAvdNB 日志里没有伴生库的那一行（日志可能已经滚掉了；冷启动后立刻看最准）' }
    } else {
        Check 'ModAvdNB 没有新日志' (-not ($text -match 'shim-ctor')) $(if ($text -match 'shim-ctor') { '还有 shim-ctor，属性可能没改干净' } else { '干净' })
    }
    Note "ModAvdNB 日志存到 $nbLog"
    if ($script:Marker) {
        $hasMarker = Test-Path -LiteralPath $script:Marker
        if ($ExpectInstalled) {
            Check "开机标记 $script:MarkerName" $hasMarker $(if ($hasMarker) { '在：照常开机也按可写系统镜像开' } else { '没有：照常开机（不带 -writable-system）时桥会消失，跑 install 补上' })
        } elseif ($hasMarker) {
            Warn "实例目录里还有开机标记 $script:MarkerName：这台实例会一直按可写系统镜像开机（无害）；uninstall 会删掉它。"
        }
    } else {
        Warn '不知道实例目录，没核对开机标记。'
    }
    $cfg = Get-AvdConfig
    if ($cfg -and (Test-Path -LiteralPath $cfg)) {
        $gm = Select-String -LiteralPath $cfg -Pattern '^hw\.gltransport\s*=\s*(\S*)' | Select-Object -First 1
        $gv = '默认（pipe）'; if ($gm) { $gv = $gm.Matches[0].Groups[1].Value }
        Note "GL 传输（config.ini，冷启动时生效）= $gv"
    }
    if ($ExpectInstalled) {
        $gt = Pv $P 'gate_target'
        Note ("modavd.gate.target = {0}" -f $(if ($gt) { "$gt（覆盖了内置清单）" } else { '(未设) —— 用伴生库里编译进去的内置允许清单；清单外的应用行为与 stock 一致' }))
    }
    if ($script:Failed) { return $false }
    return $true
}

# adb wait-for-device 没有上限，模拟器中途退出就一直等下去（10-30 升级回归里 qemu 在重启时崩了，
# 脚本挂了 7 个小时）。改成每 2 秒问一次 get-state，最多等 $TimeoutSec 秒。
function Wait-Device([int]$TimeoutSec) {
    $t0 = Get-Date
    while ($true) {
        $s = Invoke-Device @('get-state')
        if ($s.Code -eq 0 -and $s.Out -eq 'device') { return $true }
        if (((Get-Date) - $t0).TotalSeconds -ge $TimeoutSec) { return $false }
        Start-Sleep -Seconds 2
    }
}

function Wait-Boot([int]$TimeoutSec) {
    Note "等开机完成（上限 $TimeoutSec 秒）"
    if (-not (Wait-Device $TimeoutSec)) {
        Warn "等了 $TimeoutSec 秒，adb 还是连不上 $Serial（模拟器可能已经退出）"
        return $false
    }
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $TimeoutSec) {
        $r = Invoke-Guest 'getprop sys.boot_completed'
        if ($r.Out.Trim() -eq '1') {
            Note ("开机完成，用了 {0:N0} 秒" -f ((Get-Date) - $t0).TotalSeconds)
            Start-Sleep -Seconds 5
            return $true
        }
        Start-Sleep -Seconds 3
    }
    Warn "等了 $TimeoutSec 秒还没开完机"
    return $false
}

function Invoke-SyncReboot {
    Note 'adb shell sync（重启 / 关机前一定先 sync，否则刚写的文件可能变成 0 字节）'
    $s = Invoke-Guest 'sync'
    if ($s.Code -ne 0) { throw "sync 失败：$($s.Err)$($s.Out)" }
    Note 'adb reboot'
    $r = Invoke-Device @('reboot')
    if ($r.Code -ne 0) { throw "reboot 失败：$($r.Err)$($r.Out)" }
    Start-Sleep -Seconds 5
}

function Set-GateTarget([string]$Package) {
    if (-not $Package) { return }
    Head "设 modavd.gate.target（不持久，每次开机都要重设）"
    $r = Invoke-Guest ("su 0 setprop modavd.gate.target " + $Package)
    if ($r.Code -ne 0) { Warn "setprop 失败：$($r.Err)$($r.Out)"; return }
    $v = (Invoke-Guest 'getprop modavd.gate.target').Out.Trim()
    if ($v -eq $Package) { Good "modavd.gate.target = $v" } else { Warn "设完读回来是「$v」" }
    Note "对设置之后才起的进程生效。已经在跑的游戏要先 am force-stop $Package 再开。"
}

# ---------------------------------------------------------------- 开根 + 可写系统

function Enter-Writable {
    Head '取 root、remount，确认实例是可写系统镜像'
    $r = Invoke-Device @('root')
    Note ("adb root: " + (($r.Out + ' ' + $r.Err).Trim()))
    Start-Sleep -Seconds 2
    if (-not (Wait-Device 60)) { Die "adb root 之后 60 秒还连不上 $Serial（模拟器可能已经退出），这一轮什么都没装。" 1 }
    $m = Invoke-Device @('remount')
    $mtext = ($m.Out + ' ' + $m.Err).Trim()
    Note ("adb remount: " + $mtext)
    $res = Invoke-GuestScript 'wtest.sh' ($script:WriteTestSh -replace '__GUESTDIR__', $script:GuestDir)
    $t = Parse-Report $res.Out
    $ok = ((Pv $t 'w_system_lib64') -eq '1') -and ((Pv $t 'w_system_etc_init') -eq '1') -and ((Pv $t 'w_vendor') -eq '1')
    # API 34 上一台实例第一次 remount 只是把 verity 关掉，要重启一次，之后的 remount 才挂上可写的
    # overlayfs（scratch 在实例自己的 system 覆盖层里）。这次可写开机、却还写不了，就是这种情况。
    if (-not $ok -and $mtext -match 'verity|reboot') {
        if (-not $Reboot) {
            Die @"
这台实例第一次 remount：刚把 verity 关掉，要重启一次才写得进 /system、/vendor（API 34 的规矩，只有第一次）。
这一轮什么都没装。加 -Reboot 重跑，脚本会自己先 sync 再重启、接着装：
    $(Get-RerunCommand $Action -WithReboot)
"@ 3
        }
        if ($PSCmdlet.ShouldProcess($Serial, '第一次 remount 后 sync + reboot')) {
            Head '第一次 remount 关掉了 verity，先重启一次'
            Invoke-SyncReboot
            if (-not (Wait-Boot $BootTimeoutSec)) { Die '重启后没等到开机完成，这一轮什么都没装。' 1 }
            $r = Invoke-Device @('root')
            Note ("adb root: " + (($r.Out + ' ' + $r.Err).Trim()))
            Start-Sleep -Seconds 2
            if (-not (Wait-Device 60)) { Die "adb root 之后 60 秒还连不上 $Serial（模拟器可能已经退出），这一轮什么都没装。" 1 }
            $m = Invoke-Device @('remount')
            Note ("adb remount: " + (($m.Out + ' ' + $m.Err).Trim()))
            $res = Invoke-GuestScript 'wtest.sh' ($script:WriteTestSh -replace '__GUESTDIR__', $script:GuestDir)
            $t = Parse-Report $res.Out
            $ok = ((Pv $t 'w_system_lib64') -eq '1') -and ((Pv $t 'w_system_etc_init') -eq '1') -and ((Pv $t 'w_vendor') -eq '1')
        }
    }
    Note ("写探针：/system/lib64={0} /system/etc/init={1} /vendor={2} uid={3}" -f (Pv $t 'w_system_lib64'), (Pv $t 'w_system_etc_init'), (Pv $t 'w_vendor'), (Pv $t 'uid'))
    $mounts = Pv $probe 'mounts'
    if ($mounts) { Note ("挂载：" + $mounts) }
    if ($mounts -like '*overlay*') {
        Note 'system / vendor 是 overlayfs：API 34 上 adb remount 就是这么做的，scratch 在实例自己的 system 覆盖层里，跨可写开机保留（10-29 实测）。'
    }
    if (-not $ok) {
        if ($Action -eq 'install' -and (Set-WritableMarker)) {
            Die @"
这台实例这次不是按可写系统镜像开的，/system 和 /vendor 写不了，这一轮什么都没装。

  已经在实例目录放了开机标记（见上面）。0.36.0 起的模拟器看到它就按可写系统镜像开机。
  现在把这台实例关机，再照常开机一次（AUTO-MAS 开机就行），起来以后重跑：
    $(Get-RerunCommand 'install' -WithReboot)
"@ 3
        }
        if ($Action -eq 'uninstall' -and $script:Marker -and (Test-Path -LiteralPath $script:Marker)) {
            Remove-WritableMarker
            Die @"
这台实例有开机标记，却不是按可写系统镜像开的：根目录的模拟器早于 0.36.0，不认这个标记。
已经删掉标记。照常开机时这台实例用的是原版 system 镜像，桥本来就不在，现在等于没装。
覆盖层里的文件还留着，只有按可写系统镜像开机才会出现；要连它们一起删，先升级模拟器再跑 uninstall。
"@ 3
        }
        Die @"
这台实例不是用可写系统镜像起的，/system 和 /vendor 都写不了，所以装不了也卸不了桥。

  桥装在实例自己的 system.img.qcow2 / vendor.img.qcow2 覆盖层里，只有按可写系统镜像开机才挂上。
  0.36.0 起：install 会在实例目录放开机标记 modavd-writable-system，模拟器看到它就自动按可写系统镜像开机。
  开发机上也可以直接用启动器： pwsh -NoProfile -File tools\launcher\avd-p0.ps1 -WritableSystem ...（其余参数照旧）

  卸载时如果遇到这种情况：照常开机用的是原版镜像，桥本来就不在，这台实例现在等于没装。
"@ 3
    }
    Good '可写'
}

function Clear-Stale {
    # 上次没做完留下的 .modavd-new；留着不会被加载，但会让 ls 和下次核对看着乱
    $r = Invoke-Guest 'rm -f /system/lib64/libmodavd_bridge.so.modavd-new /system/lib64/libmodavd_gate.so.modavd-new /system/etc/init/modavd_bridge.rc.modavd-new /system/lib64/.modavd-writetest /system/etc/init/.modavd-writetest'
    if ($r.Code -ne 0) { Warn "清临时文件失败：$($r.Err)$($r.Out)" }
}

function Backup-BuildProp([hashtable]$P) {
    $vp = Pv $P 'vp'
    if (-not $vp) { throw '客体里找不到 /vendor/build.prop' }
    $dest = Join-Path $script:WorkDir ("build.prop.$Serial.orig")
    $r = Invoke-Device @('pull', $vp, $dest)
    if ($r.Code -ne 0 -or -not (Test-Path -LiteralPath $dest)) { throw "拉 $vp 失败：$($r.Err)$($r.Out)" }
    $sha = Sha256File $dest
    if ((Pv $P 'vp_sha') -and $sha -ne (Pv $P 'vp_sha')) { throw "拉回来的 build.prop 和客体里的不一致（$sha vs $(Pv $P 'vp_sha')）" }
    Good "改之前的 $vp 已存到 $dest（sha256 $($sha.Substring(0,16))）"
    Note '客体里还有一份 <build.prop>.modavdbak。宿主这份连实例覆盖层被删掉都还在。'
}

# ---------------------------------------------------------------- install

function Invoke-Install {
    if ($filesCurrent -and $propInstalled -and -not $Force) {
        Head '不用动'
        # 0.35 及以前装的桥没有开机标记：补上，照常开机时桥才在
        Set-WritableMarker | Out-Null
        Set-GlTransportAsg
        if ($propRunning) {
            Good '三个文件都是当前版本，属性行已就位，而且已经生效。'
            if ($GateTarget) { Set-GateTarget $GateTarget }
            Say ''
            Say '已安装且是当前版本，什么都没改。'
            return 0
        }
        Good '三个文件都是当前版本，属性行已就位，但还没重启，所以还没生效。'
        if ($Reboot) {
            if ($PSCmdlet.ShouldProcess($Serial, 'sync + reboot')) {
                Invoke-SyncReboot
                if (Wait-Boot $BootTimeoutSec) {
                    $after = Get-Probe
                    if (Invoke-Verify $after $true) { Set-GateTarget $GateTarget; Say ''; Say '装好了，核对全过。'; return 0 }
                    Say ''; Say '核对没全过，见上面的 x 行。'; return 4
                }
                return 1
            }
            return 0
        }
        Say ''
        Say "重启这台实例让它生效（先 sync）："
        Say "  `"$script:AdbExe`" -P $AdbPort -s $Serial shell sync"
        Say "  `"$script:AdbExe`" -P $AdbPort -s $Serial reboot"
        Say "起来之后核对： pwsh -NoProfile -File `"$PSCommandPath`" verify -Serial $Serial"
        return 0
    }

    Enter-Writable
    Clear-Stale

    Head '放文件（放完才改属性：属性是唯一的开关，最后一步才打开）'
    foreach ($item in $script:Items) {
        $src = $payloadFiles[$item.Key]
        $gs = Pv $probe ($item.Key + '_sha')
        if (-not $Force -and $gs -eq $hostSha[$item.Key] -and (Pv $probe ($item.Key + '_mode')) -eq '644' -and (Pv $probe ($item.Key + '_ctx')) -eq $item.Context) {
            Good "$($item.Name) 已经是当前版本，跳过"
            continue
        }
        if (-not $PSCmdlet.ShouldProcess("$Serial : $($item.Target)", 'push + chcon + chmod + mv')) { continue }
        # 先推成 <目标>.modavd-new 再改名：改名是原子的，不会把正在被别的进程映射着的
        # 旧文件截断掉（重装时这一点是必须的），也不会丢 chcon 打上的标签。
        $tmp = $item.Target + '.modavd-new'
        $push = Invoke-Device @('push', $src, $tmp)
        if ($push.Code -ne 0) { throw "push $($item.Name) -> $tmp 失败：$($push.Err)$($push.Out)" }
        $fix = Invoke-Guest ("chmod 644 {0}; chcon {1} {0}; sha256sum {0}; ls -Z {0}; stat -c %a {0}" -f $tmp, $item.Context)
        if ($fix.Code -ne 0) { throw "给 $tmp 打标签失败：$($fix.Err)$($fix.Out)" }
        if ($fix.Out -notmatch [regex]::Escape($hostSha[$item.Key])) {
            Invoke-Guest ("rm -f " + $tmp) | Out-Null
            throw "推过去的 $($item.Name) 哈希不对，期望 $($hostSha[$item.Key])；客体说：$($fix.Out)"
        }
        $mv = Invoke-Guest ("mv -f {0} {1}" -f $tmp, $item.Target)
        if ($mv.Code -ne 0) { throw "改名 $tmp -> $($item.Target) 失败：$($mv.Err)$($mv.Out)" }
        Good "$($item.Name) -> $($item.Target)"
    }

    # 改属性之前，三个文件必须全部核对通过。顺序错了就会出现「属性指向不存在的 shim」。
    Head '改属性之前再核对一遍三个文件'
    $mid = Get-Probe
    $allThere = $true
    foreach ($item in $script:Items) {
        $gs = Pv $mid ($item.Key + '_sha')
        $ok = ($gs -eq $hostSha[$item.Key]) -and ((Pv $mid ($item.Key + '_mode')) -eq '644') -and ((Pv $mid ($item.Key + '_ctx')) -eq $item.Context)
        if ($ok) { Good ("{0}  {1}  {2}" -f $item.Name, (Pv $mid ($item.Key + '_mode')), (Pv $mid ($item.Key + '_ctx'))) }
        else { Bad ("{0}  sha={1} mode={2} ctx={3}" -f $item.Name, $gs, (Pv $mid ($item.Key + '_mode')), (Pv $mid ($item.Key + '_ctx'))); $allThere = $false }
    }
    if (-not $allThere) {
        if ($WhatIfPreference) { Note '（-WhatIf：文件没真推过去，所以这里对不上是正常的）' }
        else { Die '三个文件没有全部就位，不改属性。实例保持原样（没有属性行就等于桥没装）。' 1 }
    }

    Head "往 /vendor/build.prop 追加 $script:PropLine"
    if ($PSCmdlet.ShouldProcess("$Serial : $(Pv $mid 'vp')", "备份后追加一行并核对")) {
        Backup-BuildProp $mid
        if ([int]('0' + (Pv $mid 'vp_any')) -gt 0 -and (Pv $mid 'vp_ours') -eq '0') {
            Warn ("这个文件里原本就有 native.bridge 的属性行（共 " + (Pv $mid 'vp_any') + " 条）。我们追加在最后，最后一条生效。")
        }
        $res = Invoke-GuestScript 'prop-add.sh' $script:PropAddSh
        foreach ($l in ($res.Out -split "`n")) { if ($l.Trim()) { Note $l.Trim() } }
        if ($res.Code -ne 0 -or $res.Out -match '(?m)^FAIL:') {
            Die ("追加属性行失败，客体脚本已经把 build.prop 还原。实例可以照旧开机。`n{0}{1}" -f $res.Err, $res.Out) 1
        }
        Good '属性行已就位，而且核对过了（前缀逐字节没变，尾巴正好是我们那一行）'
    }

    Head '放开机标记（照常开机也按可写系统镜像开，桥才挂得上）'
    Set-WritableMarker | Out-Null
    Head 'GL 传输换成 asg（这台实例自己的设置，下次冷启动生效）'
    Set-GlTransportAsg

    if ($Reboot) {
        if ($PSCmdlet.ShouldProcess($Serial, 'sync + reboot')) {
            Head '重启让属性生效'
            Invoke-SyncReboot
            if (-not (Wait-Boot $BootTimeoutSec)) { return 1 }
            $after = Get-Probe
            $ok = Invoke-Verify $after $true
            Set-GateTarget $GateTarget
            Say ''
            if ($ok) { Say '装好了，核对全过。'; return 0 }
            Say '核对没全过，见上面的 x 行。要回到 stock 就跑 uninstall。'
            return 4
        }
        return 0
    }

    if ($PSCmdlet.ShouldProcess($Serial, 'adb shell sync')) {
        $sync = Invoke-Guest 'sync'
        if ($sync.Code -ne 0) { Warn "sync 失败：$($sync.Err)$($sync.Out)" }
    }
    Say ''
    if ($WhatIfPreference) {
        Say '-WhatIf：上面「What if」那几行就是真跑时会做的写入，这次什么都没改。'
        Say '去掉 -WhatIf 才会真装；装完要重启这台实例（属性是开机时读的）。'
        return 0
    }
    Say '文件和属性都就位了，**还没生效**，要重启这台实例（属性是开机时读的）。'
    Say "  `"$script:AdbExe`" -P $AdbPort -s $Serial shell sync"
    Say "  `"$script:AdbExe`" -P $AdbPort -s $Serial reboot"
    Say "起来之后核对： pwsh -NoProfile -File `"$PSCommandPath`" verify -Serial $Serial"
    Say ''
    Say '⚠ 退回更早的模拟器版本之前，先跑一次 uninstall。原因见 Get-Help 的说明。'
    return 0
}

# ---------------------------------------------------------------- uninstall

function Invoke-Uninstall {
    Head '卸载顺序：先属性（这一条就足以让实例回到 stock），再 .rc，最后两个 .so'
    Enter-Writable

    if ($PSCmdlet.ShouldProcess("$Serial : $(Pv $probe 'vp')", '备份后删掉属性行并核对')) {
        Backup-BuildProp $probe
        $res = Invoke-GuestScript 'prop-del.sh' ($script:PropDelSh -replace '__GUESTDIR__', $script:GuestDir)
        foreach ($l in ($res.Out -split "`n")) { if ($l.Trim()) { Note $l.Trim() } }
        if ($res.Code -ne 0 -or $res.Out -match '(?m)^FAIL:') {
            Die ("删属性行失败。/vendor/build.prop 没动（或已还原），实例可以照旧开机。`n宿主备份：{0}`n{1}{2}" -f (Join-Path $script:WorkDir "build.prop.$Serial.orig"), $res.Err, $res.Out) 1
        }
        Good '属性行已经没了。就算后面几步全失败，这台实例下次开机也是 stock。'
    }

    foreach ($t in @('/system/etc/init/modavd_bridge.rc', '/system/lib64/libmodavd_gate.so', '/system/lib64/libmodavd_bridge.so')) {
        if (-not $PSCmdlet.ShouldProcess("$Serial : $t", 'rm -f')) { continue }
        $r = Invoke-Guest ("rm -f {0} {0}.modavd-new" -f $t)
        if ($r.Code -ne 0) { Warn "删 $t 失败：$($r.Err)$($r.Out)（属性已经去掉了，不影响回到 stock）" }
        else { Good "删掉 $t" }
    }
    Clear-Stale

    Head '撤掉装桥时改的 GL 传输，删开机标记'
    Restore-GlTransport
    Remove-WritableMarker

    $vp = Pv $probe 'vp'
    Note "客体里的 stock 备份 $vp.modavdbak 留着不动（它是参照；要删自己删）。"

    if ($Reboot) {
        if ($PSCmdlet.ShouldProcess($Serial, 'sync + reboot')) {
            Head '重启回到 stock'
            Invoke-SyncReboot
            if (-not (Wait-Boot $BootTimeoutSec)) { return 1 }
            $after = Get-Probe
            $ok = Invoke-Verify $after $false
            Say ''
            if ($ok) { Say '卸干净了，核对全过：这台实例和没装过桥一样，现在可以换包 / 退回更早的版本了。'; return 0 }
            Say '核对没全过，见上面的 x 行。属性已经去掉了，所以就算文件没删净也不影响开机。'
            return 4
        }
        return 0
    }

    if ($PSCmdlet.ShouldProcess($Serial, 'adb shell sync')) {
        $sync = Invoke-Guest 'sync'
        if ($sync.Code -ne 0) { Warn "sync 失败：$($sync.Err)$($sync.Out)" }
    }
    Say ''
    Say '卸载的写入做完了，重启这台实例才算回到 stock：'
    Say "  `"$script:AdbExe`" -P $AdbPort -s $Serial shell sync"
    Say "  `"$script:AdbExe`" -P $AdbPort -s $Serial reboot"
    Say "起来之后核对： pwsh -NoProfile -File `"$PSCommandPath`" verify -Serial $Serial"
    return 0
}

# ---------------------------------------------------------------- 入口

$exit = 0
try {
    if ($Action -eq 'install') { $exit = Invoke-Install }
    elseif ($Action -eq 'uninstall') { $exit = Invoke-Uninstall }
    else {
        $expect = ($propRunning -or $propInstalled -or $shimPresent)
        if (-not $expect) { Note '这台实例没装桥，属性也没改。下面核对的是「完全 stock」。要装就跑 install。' }
        $ok = Invoke-Verify $probe $expect
        if ($GateTarget) { Set-GateTarget $GateTarget }
        Say ''
        if ($ok) { Say '核对全过。' } else { Say '核对没全过，见上面的 x 行。'; $exit = 4 }
    }
} catch {
    Write-Host ''
    Write-Host ("出错：" + $_.Exception.Message) -ForegroundColor Red
    Write-Host '实例的 /vendor/build.prop 要么没动过，要么已经被客体脚本还原；两种情况都能照旧开机。'
    Write-Host ("工作目录（含宿主备份和日志）：" + $script:WorkDir)
    $exit = 1
}
exit $exit
