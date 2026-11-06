# Copyright 2026 qiyinxi. SPDX-License-Identifier: AGPL-3.0-or-later.
# ModAVD#35 / #96 on-device test: one fixed download window (Arknights resource download).
# Sets the arm (active = deferral on, observe = filter on but every fsync runs, off = module not armed at all:
# modavd.fsync.defer=none), force-stops Arknights, wipes its downloaded resources (Bundles + HGGameUpdate, so
# every window downloads the same opening part of the ~12.8 GB package), relaunches it, taps the two download
# dialogs ("下载完整资源" -> 确认, voice packs -> 确认) until a *.downloading appears, then runs dl-measure.sh in
# the guest. Writes <Out>\<Label>.log, .guest.txt, -ts\ (SurfaceFlinger timestats), .end.png; summarize with
# dl-sum.py. Needs: a test instance with the bridge installed and Arknights installed (user agreement accepted
# once by hand), adb root, 1920x1080 (the dialog tap points below are for that size).
#   pwsh -File dl-window.ps1 -Arm active -Label w1 -AdbPort <private adb server> -Serial emulator-<console> -Out <dir>
param(
    [Parameter(Mandatory)][ValidateSet('active', 'observe', 'off')][string]$Arm,
    [Parameter(Mandatory)][string]$Label,
    [Parameter(Mandatory)][int]$AdbPort,
    [Parameter(Mandatory)][string]$Serial,
    [Parameter(Mandatory)][string]$Out,
    [string]$Adb = 'adb',
    [int]$Secs = 150,
    [int]$Warmup = 20,
    [switch]$NoWipe
)
$ErrorActionPreference = 'Stop'
$adb = $Adb
$A = @('-P', "$AdbPort", '-s', $Serial)
$pkg = 'com.hypergryph.arknights'
$F = "/data/media/0/Android/data/$pkg/files"
$wd = $Out
New-Item -ItemType Directory -Force $wd | Out-Null
$logFile = Join-Path $wd "$Label.log"

function Log([string]$m) { $l = "$(Get-Date -Format HH:mm:ss) $m"; $l; Add-Content -LiteralPath $logFile -Value $l -Encoding utf8 }
function G([string]$cmd) { & $adb @A shell $cmd 2>&1 }
function HostLoad {
    $cpu = [int](Get-Counter '\Processor(_Total)\% Processor Time').CounterSamples[0].CookedValue
    $q = @(Get-Process qemu-system-x86_64* -ErrorAction SilentlyContinue).Count
    $c = [math]::Round((Get-Counter '\Memory\Committed Bytes').CounterSamples[0].CookedValue / 1GB, 1)
    $lim = [math]::Round((Get-Counter '\Memory\Commit Limit').CounterSamples[0].CookedValue / 1GB, 1)
    "cpu=$cpu% qemu=$q commitGB=$c/$lim"
}
# Windows System log: disk 153 (I/O retried) / UASPStor 129 (USB storage reset) -- a flaky host disk would
# distort the numbers; 0 elsewhere.
function UsbEvents([datetime]$since) {
    @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $since } -ErrorAction SilentlyContinue |
        Where-Object { ($_.ProviderName -eq 'disk' -and $_.Id -eq 153) -or ($_.ProviderName -match 'UASPStor' -and $_.Id -eq 129) }).Count
}

$t0 = Get-Date
Log "=== $Label arm=$Arm secs=$Secs warmup=$Warmup wipe=$(-not $NoWipe); host $(HostLoad); host disk reset events last 5 min: $(UsbEvents $t0.AddMinutes(-5))"
if ($Arm -eq 'off') {
    G 'setprop modavd.fsync.defer none' | Out-Null
} else {
    G "setprop modavd.fsync.defer $pkg; setprop modavd.fsync.defer.mode $Arm" | Out-Null
}
G 'setprop modavd.fsync.log 0' | Out-Null
$cs = (G 'cat /sys/devices/system/clocksource/clocksource0/current_clocksource' | Out-String).Trim()
Log "clocksource=$cs props: $((G 'getprop | grep modavd.fsync' | Out-String).Trim() -replace '\s+', ' ')"
if ($cs -ne 'tsc') { Log 'INVALID: clocksource is not tsc'; exit 3 }
if ($NoWipe) {
    G "am force-stop $pkg" | Out-Null
} else {
    G "am force-stop $pkg; rm -rf $F/Bundles $F/HGGameUpdate; sync; echo 3 > /proc/sys/vm/drop_caches; du -sk $F" | ForEach-Object { Log "  wiped: $_" }
}
& $adb @A push (Join-Path $PSScriptRoot 'dl-measure.sh') /data/local/tmp/t96/dl-measure.sh 2>&1 | Out-Null
Start-Sleep 5
G "logcat -c" | Out-Null
G "monkey -p $pkg -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1" | Out-Null
$launch = Get-Date
Log 'launched; tapping 确认 until a *.downloading appears'
& $adb @A push (Join-Path $PSScriptRoot 'px.sh') /data/local/tmp/t96/px.sh 2>&1 | Out-Null
function IsBlue([string]$line) { $v = @(($line -split ' ')[1..3] | ForEach-Object { [int]$_ }); $v.Count -eq 3 -and $v[2] -gt 110 -and ($v[2] - $v[0]) -gt 70 -and $v[1] -gt 80 -and $v[1] -lt 125 }
$started = $false
while (((Get-Date) - $launch).TotalSeconds -lt 180) {
    Start-Sleep 4
    $first = (G "find $F/HGGameUpdate/HGDownload -name '*.downloading' 2>/dev/null | head -1" | Out-String).Trim()
    if ($first) { $started = $true; break }
    # dialog 1 (下载完整资源 / 基础资源): 确认 at 1148,670; dialog 2 (配音包): 确认 at 960,899.
    $px = @(G 'sh /data/local/tmp/t96/px.sh 1148 670 960 899 624 115 624 253 624 697')
    if ($px.Count -lt 5) { continue }
    if (IsBlue $px[0]) {
        Log "  dialog 1 (resources): tap 确认"; G 'input tap 1148 670' | Out-Null
    } elseif (IsBlue $px[1]) {
        Log "  dialog 2 (voice packs): checkboxes CN=$(($px[2] -split ' ')[1]) JP=$(($px[3] -split ' ')[1]) custom=$(($px[4] -split ' ')[1]) (164 = ticked, 47 = not); tap 确认"
        G 'input tap 960 899' | Out-Null
    }
}
if (-not $started) { Log 'NO DOWNLOAD within 180 s'; exit 2 }
Log "download started $([int]((Get-Date) - $launch).TotalSeconds) s after launch"
$guestOut = Join-Path $wd "$Label.guest.txt"
G "sh /data/local/tmp/t96/dl-measure.sh $Label $Secs $Warmup 30" | Tee-Object -FilePath $guestOut | ForEach-Object { Log "  guest: $_" }
$tsDir = Join-Path $wd "$Label-ts"
New-Item -ItemType Directory -Force $tsDir | Out-Null
& $adb @A pull "/data/local/tmp/t96/dl-$Label/." $tsDir 2>&1 | Out-Null
G 'screencap -p /data/local/tmp/t96/end.png' | Out-Null
& $adb @A pull /data/local/tmp/t96/end.png (Join-Path $wd "$Label.end.png") 2>&1 | Out-Null
$ev = UsbEvents $t0
Log "done; host $(HostLoad); host disk reset events during window: $ev"
if ($ev -gt 10) { Log 'STOP: more than 10 host disk retry/reset events during the window (host disk trouble, not the module)'; exit 4 }
