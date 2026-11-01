# 回归项：用组包目录新建实例，核对随包的宿主输入法（正式签名）开机自动装好、设为默认，跑通道 20 项，再核对开发签名换成正式签名。
# 根目录照 upgrade.ps1 搭：组包目录整个复制一份（原件不动），adb、系统镜像链到 sdk-official，代替「补全组件」的下载；
# mas-avd.json 只写私有端口。开关机一律用包里的 tools\modavd.exe（关机先正常关机、不成再 sync 后结束）。
#  a. modavd create 后第一次 modavd start（无头，和 AUTO-MAS 一样；首次初始化后自己装执行桥，中间关机再开机一两次）：
#     org.modavd.ime 装上了，base.apk 和包里 components\modavd-ime.apk 逐字节相同（也就是正式签名），已启用，
#     默认输入法是它，标记 /data/local/tmp/modavd-ime.autoselected 在；
#  b. 关机后带窗口开机（文字模式要窗口）：输入法一点没变、开机日志是「已是最新」；再用 ime-reg.ps1 -Attach 跑通道 20 项，
#     测试 APK 只在这一步装、跑完卸掉；
#  c. 换签名：卸掉 org.modavd.ime，装开发签名的同一个输入法（证书和正式的不同），启用、设为默认，标记留着；关机再开机：
#     应该换回正式签名的（base.apk 逐字节相同），默认是它，标记在。再关机开机一次：什么都不变（稳态）；
#     c 没换成正式签名时，稳态记成「在开发签名的那份上测的」，不算正式签名的稳态；
#  d. modavd stop，删掉实例和根目录（-Keep 留着）。关机、删实例、等根目录下的进程退出、删根目录，哪步不成都记进问题；
#     等不到进程退出就不删根目录。日志在 <Work>\logs-<时间>\，不删。
# 用法：pwsh -NoProfile -File tools\regress\items\ime-pkg.ps1 -Package <发版输出>\mas36\release\ModAVD-0.36.0-win-x64
# 没给的路径取本机配置（tools\local.example.psd1 的 Regress 一节）。
param(
    # 组包目录（release\ModAVD-<版本>-win-x64），只读
    [Parameter(Mandatory)][string]$Package,
    # 默认 <TempRoot>\regress-ime-pkg
    [string]$Work,
    # 官方 SDK，根目录的 platform-tools、system-images 链到它（Regress.OfficialSdk）
    [string]$Official,
    [int]$Index = 9,
    # 根目录私有 adb server 的端口
    [int]$AdbPort = 20093,
    # 脚本用的 adb server。不能落在本实例的控制台 / adb / gRPC 端口上：9 号是 20090–20092，upgrade.ps1 那样取 AdbPort - 1 会撞 gRPC
    [int]$ScriptAdbPort = $AdbPort + 1,
    # 测试 APK（org.modavd.ime.testapp，ModAVDHostIME 编的，不随包发）和开发签名的同一个输入法
    # （Regress.ImeTestApk、Regress.DevImeApk）
    [string]$TestApk,
    [string]$DevImeApk,
    # Android SDK build-tools 36.1.0（apksigner 看证书；Regress.BuildTools）
    [string]$BuildTools,
    [switch]$Keep
)
$ErrorActionPreference = 'Stop'
[cultureinfo]::CurrentCulture = [cultureinfo]::InvariantCulture
. "$PSScriptRoot\common.ps1"
if (-not $Work) { $Work = Join-Path (Use-Local TempRoot $null '-Work') 'regress-ime-pkg' }
$Official = Use-Local Regress.OfficialSdk $Official '-Official'
$TestApk = Use-Local Regress.ImeTestApk $TestApk '-TestApk'
$DevImeApk = Use-Local Regress.DevImeApk $DevImeApk '-DevImeApk'
$BuildTools = Use-Local Regress.BuildTools $BuildTools '-BuildTools'
# 正式签名密钥的证书（ModAVD 的发布密钥；密钥一旦发出去就不能换）
$ReleaseCert = 'd18cca3c1982149349f164d28e02490b2eb694e44d56834fa02a1062eb42e823'
$ImeId = 'org.modavd.ime/.HostImeService'
$root = Join-Path $Work 'root'
$consolePort = 20000 + 10 * $Index
$serial = "emulator-$consolePort"
$runDir = Join-Path $Work "logs-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
$cliExe = Join-Path $root 'tools\modavd.exe'
$adbExe = Join-Path $root 'sdk\platform-tools\adb.exe'
function Modavd { & $cliExe --root $root @args 2>&1 | ForEach-Object { "$_" } }
function Adb { & $adbExe -P $AdbPort -s $serial @args 2>&1 | ForEach-Object { "$_" } }
function Log([string]$m) {
    $l = "$(Get-Date -Format 'HH:mm:ss') $m"
    if (Test-Path -LiteralPath $runDir) { Add-Content -LiteralPath (Join-Path $runDir 'ime-pkg.log') -Value $l -Encoding utf8 }
    Write-Host $l
}
# 推成文件再 sh（shell 用户就够），免得引号穿过 adb shell
function GuestSh([string]$Script) {
    $tmp = Join-Path $runDir "guest-$PID.sh"
    [IO.File]::WriteAllText($tmp, ($Script -replace "`r`n", "`n"))
    Adb push $tmp /data/local/tmp/ime-pkg.sh | Out-Null
    Remove-Item -LiteralPath $tmp -Force
    Adb shell 'sh /data/local/tmp/ime-pkg.sh; rm -f /data/local/tmp/ime-pkg.sh'
}
function CertOf([string]$Apk) {
    $hit = & (Join-Path $BuildTools 'apksigner.bat') verify --print-certs $Apk 2>&1 | ForEach-Object { "$_" } |
        Select-String -Pattern 'Signer #1 certificate SHA-256 digest: ([0-9a-f]{64})' | Select-Object -First 1
    if ($hit) { $hit.Matches[0].Groups[1].Value } else { '?' }
}
$stateScript = @'
p=$(pm path org.modavd.ime 2>/dev/null | head -n 1)
echo "path=${p#package:}"
echo "def=$(settings get secure default_input_method)"
if ime list -s 2>/dev/null | grep -qx 'org.modavd.ime/.HostImeService'; then echo enabled=1; else echo enabled=0; fi
if [ -e /data/local/tmp/modavd-ime.autoselected ]; then echo mark=1; else echo mark=0; fi
dumpsys package org.modavd.ime 2>/dev/null | grep -E 'versionCode=|firstInstallTime=|lastUpdateTime=' | head -n 3 | sed 's/^ *//; s/^/pkg=/'
'@
# 客体里输入法的现状；装着就把 base.apk 拉回来算 sha256、看证书
function Snapshot([string]$Tag) {
    $h = [ordered]@{ path = ''; def = ''; enabled = ''; mark = ''; versionCode = ''; firstInstallTime = ''; lastUpdateTime = ''; sha = ''; cert = '' }
    foreach ($l in (GuestSh $stateScript)) {
        if ($l -match '^(path|def|enabled|mark)=(.*)$') { $h[$matches[1]] = $matches[2].Trim() }
        elseif ($l -match '^pkg=(versionCode|firstInstallTime|lastUpdateTime)=(.*)$') { $h[$matches[1]] = $matches[2].Trim() }
    }
    if ($h.path) {
        $local = Join-Path $runDir "base-$Tag.apk"
        Adb pull $h.path $local | Out-Null
        if (Test-Path -LiteralPath $local) { $h.sha = (Get-FileHash -LiteralPath $local).Hash.ToLower(); $h.cert = CertOf $local }
    }
    $s = [pscustomobject]$h
    $s | ConvertTo-Json -Compress | Set-Content -LiteralPath (Join-Path $runDir "state-$Tag.json") -Encoding utf8
    $s
}
function ShowState($s) { "base.apk sha256 $(if ($s.sha) { $s.sha.Substring(0, 12) } else { '-' })…（证书 $(if ($s.cert) { $s.cert.Substring(0, 8) } else { '-' })…），启用 $($s.enabled)，默认 $(if ($s.def) { $s.def } else { '没设' })，标记 $($s.mark)" }
# 正式签名的输入法就绪：没问题返回空
function ReadyProblems($s, [string]$WantSha) {
    $bad = @()
    if (-not $s.path) { $bad += 'org.modavd.ime 没装' }
    elseif ($s.sha -ne $WantSha) { $bad += "装着的 base.apk 和包里的不同（sha256 $($s.sha)，证书 $($s.cert)）" }
    if ($s.enabled -ne '1') { $bad += '没启用' }
    if ($s.def -ne $ImeId) { $bad += "默认输入法是 $(if ($s.def) { $s.def } else { '没设' })" }
    if ($s.mark -ne '1') { $bad += '没有标记' }
    $bad
}
$sameKeys = 'path', 'sha', 'def', 'enabled', 'mark', 'versionCode', 'firstInstallTime', 'lastUpdateTime'
function Changed($Before, $After) { @($sameKeys | Where-Object { "$($Before.$_)" -ne "$($After.$_)" } | ForEach-Object { "$_ $($Before.$_) -> $($After.$_)" }) }
# modavd start；输出存成 start-<tag>.txt，返回退出码、秒数和「宿主输入法」那几行
function Boot([string]$Tag, [switch]$Window) {
    $argv = @('start', "$Index") + @(if ($Window) { '--window' })
    $t0 = Get-Date
    $o = Modavd @argv
    $code = $LASTEXITCODE
    $o | Set-Content -LiteralPath (Join-Path $runDir "start-$Tag.txt") -Encoding utf8
    $ime = @($o | Where-Object { $_ -match '宿主输入法' } | ForEach-Object { ($_ -replace '^.*?(实例 \d+ 宿主输入法)', '$1').Trim() })
    [pscustomobject]@{ Code = $code; Sec = [int]((Get-Date) - $t0).TotalSeconds; Ime = $ime; Tail = (($o | Select-Object -Last 3) -join ' / ') }
}
function Halt([string]$Tag) {
    $o = Modavd stop $Index
    $code = $LASTEXITCODE
    $o | Set-Content -LiteralPath (Join-Path $runDir "stop-$Tag.txt") -Encoding utf8
    if ($code -ne 0) { throw "modavd stop 退出码 $code：$(($o | Select-Object -Last 2) -join ' / ')" }
    ($o | Select-Object -Last 1)
}
# 模拟器带窗口要等的进程、文件锁：根目录下还有进程（私有 adb server 除外）就等，最多 $Seconds 秒
function WaitRootIdle([int]$Seconds = 90) {
    $t0 = Get-Date
    $tools = Join-Path $root 'sdk\platform-tools'
    while ($true) {
        $busy = @(Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) -and
                -not $_.ExecutablePath.StartsWith($tools, [StringComparison]::OrdinalIgnoreCase) })
        if (-not $busy) { return $true }
        if (((Get-Date) - $t0).TotalSeconds -ge $Seconds) { Log "等了 $Seconds 秒，根目录下还有进程：$(($busy | ForEach-Object { "$($_.Name)($($_.ProcessId))" }) -join '、')"; return $false }
        Start-Sleep 2
    }
}

$problems = @(); $notes = @(); $made = $false; $created = $false; $swapped = $false
try {
    # ---- 前提：组包目录、两个 APK 的签名、端口
    if (Test-Path -LiteralPath $root) { throw "$root 已存在（上次留下的？删掉或换 -Work）" }
    if (-not (Test-Path -LiteralPath (Join-Path $Package 'sdk\emulator\emulator.exe'))) { throw "不是组包目录：$Package" }
    $pkgApk = Join-Path $Package 'components\modavd-ime.apk'
    foreach ($f in $pkgApk, (Join-Path $Package 'tools\modavd.exe'), $TestApk, $DevImeApk, (Join-Path $BuildTools 'apksigner.bat')) {
        if (-not (Test-Path -LiteralPath $f)) { throw "找不到 $f" }
    }
    $mine = $consolePort..($consolePort + 2)
    foreach ($p in $AdbPort, $ScriptAdbPort) {
        if ($p -in 5037, 20049, 20050) { throw "adb server 端口 $p 是生产用的，不碰" }
        if ($p -in $mine) { throw "adb server 端口 $p 和 $Index 号实例的控制台 / adb / gRPC 端口（$($mine -join '、')）撞了" }
    }
    if ($AdbPort -eq $ScriptAdbPort) { throw '私有和脚本用的 adb server 端口不能一样' }
    $busyPorts = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -in (@($AdbPort, $ScriptAdbPort) + $mine) } | ForEach-Object { $_.LocalPort } | Sort-Object -Unique)
    if ($busyPorts) { throw "端口已被占用：$($busyPorts -join '、')" }
    New-Item -ItemType Directory -Force $runDir | Out-Null
    $buildId = (Select-String -LiteralPath (Join-Path $Package 'sdk\emulator\source.properties') -Pattern '^Pkg\.BuildId=(.+)$').Matches[0].Groups[1].Value.Trim()
    $pkgSha = (Get-FileHash -LiteralPath $pkgApk).Hash.ToLower(); $pkgCert = CertOf $pkgApk
    $devSha = (Get-FileHash -LiteralPath $DevImeApk).Hash.ToLower(); $devCert = CertOf $DevImeApk
    Log "组包目录 $Package（$buildId）；modavd-ime.apk sha256 $pkgSha，证书 $pkgCert"
    Log "开发签名的 $DevImeApk sha256 $devSha，证书 $devCert；测试 APK $TestApk"
    if ($pkgCert -ne $ReleaseCert) { throw "包里的 modavd-ime.apk 不是正式签名（证书 $pkgCert）" }
    if ($devCert -eq '?' -or $devCert -eq $ReleaseCert -or $devSha -eq $pkgSha) { throw "$DevImeApk 不是另一把密钥签的（证书 $devCert）" }

    # ---- 根目录：组包目录的副本
    $made = $true
    & robocopy $Package $root /E /COPY:DAT /R:5 /W:2 /NFL /NDL /NP /NJH /NJS | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy $Package -> $root 失败（$LASTEXITCODE）" }
    foreach ($n in 'platform-tools', 'system-images') { New-Item -ItemType Junction -Path (Join-Path $root "sdk\$n") -Target (Join-Path $Official $n) | Out-Null }
    [IO.File]::WriteAllText((Join-Path $root 'mas-avd.json'), "{`n  `"adbServerPort`": $AdbPort,`n  `"scriptAdbServerPort`": $ScriptAdbPort`n}`n", [Text.UTF8Encoding]::new($false))
    Log "根目录 $root（adb server $AdbPort / $ScriptAdbPort）"

    # ---- a. 新建、第一次开机
    $o = Modavd create --index $Index --title 输入法回归 --memory 4G --cpu 4 --data-gb 16
    if ($LASTEXITCODE -ne 0) { throw "新建失败：$($o -join ' ')" }
    $created = $true
    $b = Boot 'a-first'
    $b.Ime | ForEach-Object { Log "  a: $_" }
    if ($b.Code -ne 0) { throw "第一次开机失败（退出码 $($b.Code)）：$($b.Tail)" }
    $nb = ((Adb shell getprop ro.dalvik.vm.native.bridge) -join '').Trim()
    $sa = Snapshot 'a'
    Log "a: 第一次开机 $($b.Sec) 秒（native bridge $nb）；$(ShowState $sa)"
    $bad = @(ReadyProblems $sa $pkgSha)
    if ($bad) { $problems += "a 第一次开机后：$($bad -join '，')（$($b.Ime -join ' / ')）" }
    else { $notes += "a 第一次开机（$($b.Sec) 秒，含装桥）装上正式签名的输入法、已启用、设为默认、标记在" }

    # ---- b. 带窗口开机，通道 20 项
    Log "  $(Halt 'a')"
    $b = Boot 'b-window' -Window
    $b.Ime | ForEach-Object { Log "  b: $_" }
    if ($b.Code -ne 0) { throw "带窗口开机失败（退出码 $($b.Code)）：$($b.Tail)" }
    $sb = Snapshot 'b'
    $diff = Changed $sa $sb
    if ($diff -or -not ($b.Ime -match '已是最新')) { $problems += "b 再开机输入法变了：$($diff -join '；')（$($b.Ime -join ' / ')）" }
    $emuLog = Get-ChildItem -LiteralPath (Join-Path $root 'logs') -File | Where-Object { $_.Name -match "^mas_$Index-\d{8}-\d{6}\.log$" } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
    if (-not $emuLog) { throw '找不到这次开机的模拟器日志' }
    $regOut = Join-Path $runDir 'ime-reg'
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'ime-reg.ps1') -Attach -Sdk (Join-Path $root 'sdk') -Out $regOut -AdbExe $adbExe `
        -AdbPort $AdbPort -Serial $serial -EmulatorLog $emuLog -TestApk $TestApk 2>&1 | ForEach-Object { Write-Host "  ime-reg: $_" }
    $regLog = @(Get-Content -LiteralPath (Join-Path $regOut 'ime-reg.log') -Encoding utf8 -ErrorAction SilentlyContinue)
    $sum = $regLog | Select-String -Pattern '汇总：PASS (\d+)，FAIL (\d+)' | Select-Object -Last 1
    $fails = @($regLog | Where-Object { $_ -match ' FAIL ' } | ForEach-Object { ($_ -replace '^\S+ FAIL ', '') })
    if ($sum -and [int]$sum.Matches[0].Groups[1].Value -eq 20 -and [int]$sum.Matches[0].Groups[2].Value -eq 0) { $notes += 'b 带窗口开机通道 PASS 20，FAIL 0' }
    else { $problems += "b 通道 $(if ($sum) { $sum.Matches[0].Value } else { '没有汇总行' })$(if ($fails) { "：$($fails -join '；')" })" }
    $left = ((Adb shell pm path org.modavd.ime.testapp) -join '').Trim()
    if ($left) { $problems += "测试 APK 没卸掉（$left）" }

    # ---- c. 开发签名换成正式签名
    $r = (Adb uninstall org.modavd.ime) -join ' '
    if ($r -notmatch '\bSuccess\b') { throw "卸载 org.modavd.ime 失败：$r" }
    $r = (Adb install $DevImeApk) -join ' '
    if ($r -notmatch '\bSuccess\b') { throw "装开发签名的输入法失败：$r" }
    GuestSh @'
i=0
while [ $i -lt 40 ]; do ime list -a -s | grep -qx 'org.modavd.ime/.HostImeService' && break; sleep 0.25; i=$((i+1)); done
ime enable org.modavd.ime/.HostImeService
ime set org.modavd.ime/.HostImeService
sync
'@ | ForEach-Object { Log "  c: $_" }
    $sd = Snapshot 'c-dev'
    Log "c: 换上开发签名的：$(ShowState $sd)"
    if ($sd.sha -ne $devSha -or $sd.def -ne $ImeId -or $sd.enabled -ne '1' -or $sd.mark -ne '1') { throw "开发签名的没摆好：$(ShowState $sd)" }
    Log "  $(Halt 'c-dev')"
    $b = Boot 'c-resign'
    $b.Ime | ForEach-Object { Log "  c: $_" }
    if ($b.Code -ne 0) { throw "换签名那次开机失败（退出码 $($b.Code)）：$($b.Tail)" }
    $sc = Snapshot 'c'
    Log "c: 开机后 $(ShowState $sc)"
    $bad = @(ReadyProblems $sc $pkgSha)
    $swapped = $bad.Count -eq 0
    if (-not $swapped) { $problems += "c 开发签名的开机后没换成正式签名：$($bad -join '，')（$($b.Ime -join ' / ')）" }
    else { $notes += "c 开发签名的开机后换回正式签名、默认和标记都在（$($b.Ime -join ' / '))" }

    # 稳态：再关机开机一次，什么都不变。c 没换成正式签名时测的是客体上留着的那份，照实写
    $on = if ($swapped) { '' } elseif ($sc.sha -eq $devSha) { '开发签名的那份上' } else { "没换成正式签名的状态上（$(ShowState $sc)）" }
    Log "  $(Halt 'c')"
    $b = Boot 'c-steady'
    $b.Ime | ForEach-Object { Log "  c2: $_" }
    if ($b.Code -ne 0) { throw "稳态那次开机失败（退出码 $($b.Code)）：$($b.Tail)" }
    $s2 = Snapshot 'c2'
    $diff = Changed $sc $s2
    if ($diff -or -not ($b.Ime -match '已是最新')) { $problems += "c 稳态开机又变了$(if ($on) { "（在${on}）" })：$($diff -join '；')（$($b.Ime -join ' / ')）" }
    elseif ($swapped) { $notes += '再开机一次什么都没变（已是最新）' }
    else { $notes += "再开机一次什么都没变（已是最新），但这是在${on}测的，不算正式签名的稳态" }
} catch {
    $problems += "出错（第 $($_.InvocationInfo.ScriptLineNumber) 行）：$($_.Exception.Message)"
    Log $problems[-1]
} finally {
    # 收尾哪步不成都记进问题：留下的实例、进程、目录会挡下一次
    if ($created) {
        try { Log "  $(Halt 'end')" } catch { $problems += "收尾关机失败：$($_.Exception.Message)"; Log $problems[-1] }
        if (-not $Keep) {
            $code = -1
            for ($k = 0; $k -lt 30; $k++) { $o = Modavd delete $Index --yes; $code = $LASTEXITCODE; if ($code -eq 0) { break }; Start-Sleep 2 }
            Log "  删实例：$(($o | Select-Object -Last 1))"
            if ($code -ne 0) { $problems += "收尾删实例试了 30 次都没成（退出码 $code）：$(($o | Select-Object -Last 1))"; Log $problems[-1] }
        }
    }
    # 这个根目录的私有 adb server 跑完就关：留着会一直占着端口
    if ($made) {
        foreach ($p in $AdbPort, $ScriptAdbPort) { & (Join-Path $Official 'platform-tools\adb.exe') -P $p kill-server 2>&1 | Out-Null }
    }
    if ($made -and -not $Keep) {
        # 根目录下还有进程在跑就不删：删一半的根目录比留着更难收拾
        if (-not (WaitRootIdle)) { $problems += "收尾等了 90 秒根目录下还有进程，没删 $root"; Log $problems[-1] }
        else {
            # 先拆两个目录联接（rmdir 只删联接本身），确认根目录下没有任何重解析点了，才整个删掉：绝不能删进 sdk-official
            foreach ($n in 'platform-tools', 'system-images') { $j = Join-Path $root "sdk\$n"; if (Test-Path -LiteralPath $j) { cmd /c rmdir "$j" | Out-Null } }
            $links = @(Get-ChildItem -LiteralPath $root -Recurse -Force -Attributes ReparsePoint -ErrorAction SilentlyContinue)
            if ($links) { $problems += "收尾时根目录下还有联接，没删 $root：$($links.FullName -join '、')"; Log $problems[-1] }
            else {
                for ($k = 0; $k -lt 15 -and (Test-Path -LiteralPath $root); $k++) {
                    try { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction Stop } catch { Start-Sleep 2 }
                }
                if (Test-Path -LiteralPath $root) { $problems += "收尾删了 15 次 $root 还没删干净"; Log $problems[-1] } else { Log "删掉了 $root（日志留在 $runDir）" }
            }
        }
    }
}
$summary = $notes -join '；'
if ($problems) { $summary += "；问题：$($problems -join '；')" }
Result ($problems.Count -eq 0) $summary
exit 0
