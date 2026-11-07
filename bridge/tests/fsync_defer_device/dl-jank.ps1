# Copyright 2026 qiyinxi. SPDX-License-Identifier: AGPL-3.0-or-later.
# ModAVD#35 / #96 on-device test: frame-stall diagnosis during a download. Like dl-window.ps1 (arm, wipe, launch,
# tap dialogs), then runs jank-probe in the guest for -Secs -- heartbeat gaps of a 5 ms sleeper thread (a whole-guest
# stall shows here), *.downloading files finishing, Dirty / Writeback, and the game's frame gaps > 100 ms from
# SurfaceFlinger -- on one timeline, while the host logs a disk's write latency every second. Analyze with
# jank-sum.py (<Out>\<Label>.jank.txt). Needs jank-probe in -BinDir (build.ps1).
param(
    [Parameter(Mandatory)][ValidateSet('active', 'observe', 'off')][string]$Arm,
    [Parameter(Mandatory)][string]$Label,
    [Parameter(Mandatory)][int]$AdbPort,
    [Parameter(Mandatory)][string]$Serial,
    [Parameter(Mandatory)][string]$Out,
    [Parameter(Mandatory)][string]$BinDir,
    [string]$Adb = 'adb',
    [string]$HostDisk = '_Total',   # PhysicalDisk counter instance holding the instance's disk, e.g. '2 E:'
    [int]$Secs = 150
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
function IsBlue([string]$line) { $v = @(($line -split ' ')[1..3] | ForEach-Object { [int]$_ }); $v.Count -eq 3 -and $v[2] -gt 110 -and ($v[2] - $v[0]) -gt 70 -and $v[1] -gt 80 -and $v[1] -lt 125 }

Log "=== $Label arm=$Arm secs=$Secs (jank diagnosis)"
if ($Arm -eq 'off') { G 'setprop modavd.fsync.defer none' | Out-Null } else { G "setprop modavd.fsync.defer $pkg; setprop modavd.fsync.defer.mode $Arm" | Out-Null }
& $adb @A push (Join-Path $BinDir 'jank-probe') /data/local/tmp/t96/jank-probe 2>&1 | Out-Null
& $adb @A push (Join-Path $PSScriptRoot 'px.sh') /data/local/tmp/t96/px.sh 2>&1 | Out-Null
G "chmod 755 /data/local/tmp/t96/jank-probe; am force-stop $pkg; rm -rf $F/Bundles $F/HGGameUpdate; sync; echo 3 > /proc/sys/vm/drop_caches" | Out-Null
Start-Sleep 3
G "monkey -p $pkg -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1" | Out-Null
$launch = Get-Date
$started = $false
while (((Get-Date) - $launch).TotalSeconds -lt 180) {
    Start-Sleep 4
    if ((G "find $F/HGGameUpdate/HGDownload -name '*.downloading' 2>/dev/null | head -1" | Out-String).Trim()) { $started = $true; break }
    $px = @(G 'sh /data/local/tmp/t96/px.sh 1148 670 960 899')
    if ($px.Count -lt 2) { continue }
    if (IsBlue $px[0]) { G 'input tap 1148 670' | Out-Null } elseif (IsBlue $px[1]) { G 'input tap 960 899' | Out-Null }
}
if (-not $started) { Log 'NO DOWNLOAD within 180 s'; exit 2 }
Start-Sleep 15
$layer = (G 'dumpsys SurfaceFlinger --list | grep -F "SurfaceView[com.hypergryph.arknights" | grep -F "(BLAST)" | head -1' | Out-String).Trim()
Log "download running; layer=$layer"
# host side, same time span: the disk's write latency / throughput / queue and host CPU, every second
$ctr = @("\PhysicalDisk($HostDisk)\Avg. Disk sec/Write", "\PhysicalDisk($HostDisk)\Disk Write Bytes/sec", "\PhysicalDisk($HostDisk)\Current Disk Queue Length", '\Processor(_Total)\% Processor Time')
$hostCsv = Join-Path $wd "$Label.host.csv"
$job = Start-Job -ScriptBlock { param($c, $n, $out) Get-Counter -Counter $c -SampleInterval 1 -MaxSamples $n | ForEach-Object { $t = $_.Timestamp.ToString('HH:mm:ss.fff'); $v = $_.CounterSamples | ForEach-Object { $_.CookedValue.ToString('0.######', [Globalization.CultureInfo]::InvariantCulture) }; "$t;" + ($v -join ';') } | Set-Content $out -Encoding utf8 } -ArgumentList $ctr, ($Secs + 5), $hostCsv
Start-Sleep 1
$probeStart = Get-Date
Log "probe t=0 at host $($probeStart.ToString('HH:mm:ss.fff'))"
G "/data/local/tmp/t96/jank-probe $Secs $F/HGGameUpdate '$layer'" | Tee-Object -FilePath (Join-Path $wd "$Label.jank.txt") | Out-Null
$job | Wait-Job -Timeout 30 | Out-Null
$job | Remove-Job -Force
Set-Content (Join-Path $wd "$Label.align.txt") "probe_t0_host=$($probeStart.ToString('HH:mm:ss.fff'))" -Encoding utf8
G 'logcat -d -s ModAvdFsync | grep summary | tail -2' | ForEach-Object { Log "  fsync: $_" }
$j = Get-Content (Join-Path $wd "$Label.jank.txt")
Log ("frame gaps>100ms: {0}; heartbeat gaps>50ms: {1}; files done: {2}" -f @($j | Select-String 'FRAME gap').Count, @($j | Select-String 'HB gap').Count, @($j | Select-String 'DL done').Count)
