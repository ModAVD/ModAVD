# Copyright 2026 qiyinxi. SPDX-License-Identifier: AGPL-3.0-or-later.
# ModAVD#35 / #96 on-device test: hard-kill a test instance in the middle of Arknights' resource download, boot it
# again and check whether the game recovers by itself (no corrupted resource package, no full re-download, no hang).
#   1. set the arm, wipe resources unless -NoWipe, launch, tap the two download dialogs;
#   2. when files/ has grown to -KillAtMiB (du), snapshot the download state (du, every file under HGDownload with
#      size and mtime) and the ModAvdFsync summary -- WITHOUT a guest sync;
#   3. "adb emu kill" without sync: qemu exits at once and the guest loses its page cache = a power cut for the
#      guest. Only if qemu is still there after 30 s is its process ended (by the pid found for this instance);
#   4. boot with "<ModavdRoot>\tools\modavd.exe start <Instance>", collect fs / fsck messages, snapshot the same
#      state again, copy every file of the in-flight downloads (what the disk really holds) and zero-scan them;
#   5. set the arm again (props are not persistent), relaunch, tap dialogs, watch -WatchSecs: du every ~10 s,
#      screenshots, the game's unzip errors (zip4j "crc verification failed" = the 302 failure), and hard-link
#      each captured file the moment the game finishes it (rename), to compare its prefix with the capture.
# Output: <Out>\kill\<Label>\ (run.log, *-state.txt, logcat-before/after.txt, zscan-*.txt, compare.txt, *.png).
# Needs zscan built for the guest in -BinDir (build.ps1). Only for a disposable test instance.
param(
    [Parameter(Mandatory)][ValidateSet('active', 'observe', 'off')][string]$Arm,
    [Parameter(Mandatory)][string]$Label,
    [Parameter(Mandatory)][int]$AdbPort,
    [Parameter(Mandatory)][string]$Serial,          # emulator-<console port>
    [Parameter(Mandatory)][string]$ModavdRoot,      # ModAVD root of the instance (tools\modavd.exe)
    [Parameter(Mandatory)][int]$Instance,
    [Parameter(Mandatory)][string]$Out,
    [Parameter(Mandatory)][string]$BinDir,          # guest binaries from build.ps1 (zscan)
    [string]$Adb = 'adb',
    [int]$KillAtMiB = 3000,
    [int]$WatchSecs = 180,
    [switch]$NoWipe,
    # arm after the reboot (default: same as -Arm); the corruption question is about what was durable at the kill
    [ValidateSet('', 'active', 'observe', 'off')][string]$ResumeArm = ''
)
$ErrorActionPreference = 'Stop'
$root = $ModavdRoot
$adb = $Adb
$A = @('-P', "$AdbPort", '-s', $Serial)
$consolePort = [int]($Serial -replace '^emulator-', '')
$pkg = 'com.hypergryph.arknights'
$F = "/data/media/0/Android/data/$pkg/files"
$kd = Join-Path $Out "kill\$Label"
New-Item -ItemType Directory -Force $kd | Out-Null
$logFile = Join-Path $kd 'run.log'

function Log([string]$m) { $l = "$(Get-Date -Format HH:mm:ss) $m"; $l; Add-Content -LiteralPath $logFile -Value $l -Encoding utf8 }
function G([string]$cmd) { & $adb @A shell $cmd 2>&1 }
function HostLoad {
    $cpu = [int](Get-Counter '\Processor(_Total)\% Processor Time').CounterSamples[0].CookedValue
    $q = @(Get-Process qemu-system-x86_64* -ErrorAction SilentlyContinue).Count
    "cpu=$cpu% qemu=$q"
}
# Windows System log: disk 153 (I/O retried) / UASPStor 129 (USB storage reset) -- host disk trouble.
function UsbEvents([datetime]$since) {
    @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $since } -ErrorAction SilentlyContinue |
        Where-Object { ($_.ProviderName -eq 'disk' -and $_.Id -eq 153) -or ($_.ProviderName -match 'UASPStor' -and $_.Id -eq 129) }).Count
}
function SetArm([string]$which) {
    G 'setprop modavd.fsync.log 0' | Out-Null
    if (-not $which) { $which = $Arm }
    if ($which -eq 'off') { G 'setprop modavd.fsync.defer none' | Out-Null }
    else { G "setprop modavd.fsync.defer $pkg; setprop modavd.fsync.defer.mode $which" | Out-Null }
}
function IsBlue([string]$line) { $v = @(($line -split ' ')[1..3] | ForEach-Object { [int]$_ }); $v.Count -eq 3 -and $v[2] -gt 110 -and ($v[2] - $v[0]) -gt 70 -and $v[1] -gt 80 -and $v[1] -lt 125 }
function TapDialogs([int]$seconds, [switch]$UntilDownloading) {
    $t = Get-Date
    while (((Get-Date) - $t).TotalSeconds -lt $seconds) {
        Start-Sleep 4
        if ($UntilDownloading) {
            $first = (G "find $F/HGGameUpdate/HGDownload -name '*.downloading' 2>/dev/null | head -1" | Out-String).Trim()
            if ($first) { return $true }
        }
        $px = @(G 'sh /data/local/tmp/t96/px.sh 1148 670 960 899')
        if ($px.Count -lt 2) { continue }
        if (IsBlue $px[0]) { Log '  dialog 1 (resources): tap 确认'; G 'input tap 1148 670' | Out-Null }
        elseif (IsBlue $px[1]) { Log '  dialog 2 (voice packs): tap 确认'; G 'input tap 960 899' | Out-Null }
    }
    return $false
}
function Shot([string]$name) {
    G 'screencap -p /data/local/tmp/t96/k.png' | Out-Null
    & $adb @A pull /data/local/tmp/t96/k.png (Join-Path $kd "$name.png") 2>&1 | Out-Null
}
function State([string]$name) {
    $cmd = "echo du_kb=`$(du -sk $F | cut -f1); echo bundles_kb=`$(du -sk $F/Bundles 2>/dev/null | cut -f1); " +
        "find $F/HGGameUpdate/HGDownload -type f 2>/dev/null | while read f; do echo `"`$(stat -c '%s %Y' `"`$f`") `$f`"; done; " +
        "echo newest_bundles:; find $F/Bundles -type f -mmin -2 2>/dev/null | head -40 | while read f; do echo `"`$(stat -c '%s %Y' `"`$f`") `$f`"; done"
    G $cmd | Set-Content (Join-Path $kd "$name-state.txt") -Encoding utf8
    (Get-Content (Join-Path $kd "$name-state.txt") -TotalCount 2) -join ' '
}
function QemuPid { @(Get-CimInstance Win32_Process -Filter "Name like 'qemu-system%'" | Where-Object { $_.CommandLine -match "-ports $consolePort,$($consolePort + 1)\b" -and $_.CommandLine -match "-avd mas_$Instance\b" }) }

$t0 = Get-Date
Log "=== $Label arm=$Arm killAt=${KillAtMiB}MiB watch=${WatchSecs}s wipe=$(-not $NoWipe); host $(HostLoad)"
SetArm
& $adb @A push (Join-Path $PSScriptRoot 'px.sh') /data/local/tmp/t96/px.sh 2>&1 | Out-Null
if ($NoWipe) { G "am force-stop $pkg" | Out-Null }
else { G "am force-stop $pkg; rm -rf $F/Bundles $F/HGGameUpdate; sync; du -sk $F" | ForEach-Object { Log "  wiped: $_" } }
G 'logcat -c' | Out-Null
G "monkey -p $pkg -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1" | Out-Null
if (-not (TapDialogs 180 -UntilDownloading)) { Log 'NO DOWNLOAD within 180 s'; exit 2 }
Log 'download running; waiting for the kill point'
$deadline = (Get-Date).AddMinutes(15)
while ((Get-Date) -lt $deadline) {
    Start-Sleep 5
    $du = [int64]((G "du -sk $F | cut -f1" | Out-String).Trim())
    if ($du / 1024 -ge $KillAtMiB) { break }
}
Shot 'before-kill'
Log "before kill: $(State 'before')"
G 'logcat -d -s ModAvdFsync' | Select-Object -Last 3 | ForEach-Object { Log "  fsync: $_" }
G 'logcat -d' | Set-Content (Join-Path $kd 'logcat-before.txt') -Encoding utf8

$q = QemuPid
if ($q.Count -ne 1) { throw "expected one qemu for console $consolePort, found $($q.Count)" }
$qpid = $q[0].ProcessId
# "emu kill" WITHOUT a guest sync: qemu exits at once, the guest's page cache is lost = a power cut for the
# guest (agents\关机与硬杀.md: same result as a hard kill). Ending qemu's process is only the fallback.
Log "HARD KILL: emu kill without sync (qemu pid=$qpid)"
Add-Content (Join-Path $Out 'pids.txt') "$Label hard-kill (emu kill, no sync) qemu pid=$qpid $(Get-Date -Format s)"
& $adb @A emu kill 2>&1 | Out-Null
$w = (Get-Date).AddSeconds(30)
while ((Get-Process -Id $qpid -ErrorAction SilentlyContinue) -and (Get-Date) -lt $w) { Start-Sleep 1 }
if (Get-Process -Id $qpid -ErrorAction SilentlyContinue) { Log "  qemu still there after 30 s; ending recorded pid $qpid"; Stop-Process -Id $qpid -Force }
Log "qemu gone=$(-not (Get-Process -Id $qpid -ErrorAction SilentlyContinue))"
Start-Sleep 5

Log "boot again (modavd start $Instance)"
& "$root\tools\modavd.exe" --root $root start $Instance --timeout 300 2>&1 | Select-Object -Last 4 | ForEach-Object { Log "  start: $_" }
$q2 = QemuPid
Log "new qemu pid=$($q2.ProcessId -join ',')"
Add-Content (Join-Path $Out 'pids.txt') "$Label boot-after-kill qemu pid=$($q2.ProcessId -join ',') $(Get-Date -Format s)"
& $adb @A root 2>&1 | Out-Null
Start-Sleep 3
& $adb @A wait-for-device
$cs = (G 'cat /sys/devices/system/clocksource/clocksource0/current_clocksource' | Out-String).Trim()
Log "clocksource=$cs"
G "dmesg | grep -iE 'ext4|f2fs|fsck|recover|orphan|error|I/O' | grep -v -iE 'audit|selinux' | tail -30" | Set-Content (Join-Path $kd 'dmesg-fs.txt') -Encoding utf8
G 'cat /dev/fscklogs/log 2>/dev/null | tail -30' | Set-Content (Join-Path $kd 'fsck-log.txt') -Encoding utf8
Log "fs messages: $((Get-Content (Join-Path $kd 'dmesg-fs.txt') | Measure-Object).Count) lines in dmesg-fs.txt"
Log "after boot: $(State 'after-boot')"
# what the disk really holds for the in-flight downloads, before the game touches them again
& $adb @A push (Join-Path $BinDir 'zscan') /data/local/tmp/t96/zscan 2>&1 | Out-Null
$cd = "/data/local/tmp/t96/crash-$Label"
G "chmod 755 /data/local/tmp/t96/zscan; rm -rf $cd; mkdir -p $cd; for f in `$(find $F/HGGameUpdate/HGDownload -path '*/download/*' -type f); do cp `$f $cd/; done; /data/local/tmp/t96/zscan $cd/*; md5sum $cd/*" | Set-Content (Join-Path $kd 'zscan-after-boot.txt') -Encoding utf8
Get-Content (Join-Path $kd 'zscan-after-boot.txt') | Where-Object { $_ -match 'size|zero run' } | ForEach-Object { Log "  disk: $_" }

$ra = if ($ResumeArm) { $ResumeArm } else { $Arm }
Log "resume arm: $ra"
SetArm $ra
& $adb @A push (Join-Path $PSScriptRoot 'px.sh') /data/local/tmp/t96/px.sh 2>&1 | Out-Null
G 'logcat -c' | Out-Null
# hard-link each captured in-flight file the moment the game finishes it (rename), before unzip+delete
& $adb @A push (Join-Path $PSScriptRoot 'grab.sh') /data/local/tmp/t96/grab.sh 2>&1 | Out-Null
$gd = "/data/media/0/t96grab-$Label"   # same mount as the game's files (a hard link cannot cross mounts)
$dlDir = (G "ls -d $F/HGGameUpdate/HGDownload/file_*/download | head -1" | Out-String).Trim()
G "rm -rf $gd; mkdir -p $gd; chmod 755 /data/local/tmp/t96/grab.sh" | Out-Null
$grabJob = Start-Job -ScriptBlock { param($adb, $port, $serial, $cd, $gd, $dl, $secs) & $adb -P $port -s $serial shell "CRASH=$cd GRAB=$gd timeout $secs inotifyd /data/local/tmp/t96/grab.sh ${dl}:nmydw" } -ArgumentList $adb, "$AdbPort", $Serial, $cd, $gd, $dlDir, ($WatchSecs + 30)
Start-Sleep 2
G "monkey -p $pkg -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1" | Out-Null
Log "relaunched; watching (grabbing finished copies of the captured files from $dlDir)"
$tw = Get-Date
$i = 0
while (((Get-Date) - $tw).TotalSeconds -lt $WatchSecs) {
    TapDialogs 10 | Out-Null
    $i++
    $du = (G "du -sk $F | cut -f1; find $F -name '*.downloading' | wc -l; pidof $pkg" | Out-String).Trim() -replace '\s+', ' '
    Log "  t=$([int]((Get-Date) - $tw).TotalSeconds)s du_kb/dl_files/pids: $du"
    if ($i -in 3, 9, 17) { Shot "after-$i" }
}
Shot 'after-end'
G 'logcat -d' | Set-Content (Join-Path $kd 'logcat-after.txt') -Encoding utf8
$crc = @(Get-Content (Join-Path $kd 'logcat-after.txt') | Select-String -Pattern 'crc verification failed|ZipException' | ForEach-Object { $_.Line })
Log "game unzip errors after the kill: $($crc.Count)"
$crc | Select-Object -First 3 | ForEach-Object { Log "  ! $_" }
G "/data/local/tmp/t96/zscan `$(find $F -name '*.downloading')" | Set-Content (Join-Path $kd 'zscan-end.txt') -Encoding utf8
$grabJob | Stop-Job -ErrorAction SilentlyContinue; $grabJob | Remove-Job -Force -ErrorAction SilentlyContinue
# compare: the disk's prefix right after the reboot vs the same bytes of the game's finished file
$cmpCmd = "cat $gd/grab.log 2>/dev/null; for g in $gd/*; do [ -f `"`$g`" ] || continue; n=`${g##*/}; [ `"`$n`" = grab.log ] && continue; c=$cd/`$n.downloading; s=`$(stat -c %s `$c); " +
    "d=`$(cmp -l -n `$s `$c `$g 2>/dev/null | wc -l); f=`$(cmp -n `$s `$c `$g 2>/dev/null | head -1); echo `"`$n: captured prefix `$s bytes, final `$(stat -c %s `$g) bytes, differing bytes in prefix: `$d `$f`"; done"
G ($cmpCmd + "; for g in $gd/*; do [ -f `"`$g`" ] && /data/local/tmp/t96/zscan `$g; done") | Set-Content (Join-Path $kd 'compare.txt') -Encoding utf8
Get-Content (Join-Path $kd 'compare.txt') | ForEach-Object { Log "  compare: $_" }
G 'logcat -d -s ModAvdFsync' | Select-Object -Last 3 | ForEach-Object { Log "  fsync: $_" }
Log "done; host disk reset events since start: $(UsbEvents $t0); host $(HostLoad)"
