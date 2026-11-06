# Copyright 2026 qiyinxi. SPDX-License-Identifier: AGPL-3.0-or-later.
# Repeat dl-kill.ps1 up to -Rounds times for one arm; stop at the first round where the game reports an unzip
# (CRC) error after the kill -- the 302 failure -- and keep that round's evidence; otherwise delete the round's
# guest-side copies (crash-* / t96grab-*) so the data disk does not fill up. One line per round in
# <Out>\kill\killloop-<arm>.log. The other parameters are passed to dl-kill.ps1 unchanged.
param(
    [Parameter(Mandatory)][ValidateSet('active', 'off')][string]$Arm,
    [Parameter(Mandatory)][int]$AdbPort,
    [Parameter(Mandatory)][string]$Serial,
    [Parameter(Mandatory)][string]$ModavdRoot,
    [Parameter(Mandatory)][int]$Instance,
    [Parameter(Mandatory)][string]$Out,
    [Parameter(Mandatory)][string]$BinDir,
    [string]$Adb = 'adb',
    [int]$Rounds = 10,
    [int]$Start = 1,
    [int]$KillAtMiB = 2000,
    [int]$WatchSecs = 150
)
$sum = Join-Path $Out "kill\killloop-$Arm.log"
New-Item -ItemType Directory -Force (Split-Path $sum) | Out-Null
for ($i = $Start; $i -lt $Start + $Rounds; $i++) {
    $label = "k-$Arm-$i"
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'dl-kill.ps1') -Arm $Arm -Label $label -AdbPort $AdbPort -Serial $Serial `
        -ModavdRoot $ModavdRoot -Instance $Instance -Out $Out -BinDir $BinDir -Adb $Adb -KillAtMiB $KillAtMiB -WatchSecs $WatchSecs *> $null
    $log = Get-Content (Join-Path $Out "kill\$label\run.log") -Encoding utf8 -ErrorAction SilentlyContinue
    $err = ($log | Select-String 'game unzip errors after the kill: (\d+)' | ForEach-Object { [int]$_.Matches[0].Groups[1].Value }) | Select-Object -Last 1
    $before = ($log | Select-String 'before kill: du_kb=(\d+)').Matches.Groups[1].Value
    $after = ($log | Select-String 'after boot: du_kb=(\d+)').Matches.Groups[1].Value
    $last = ($log | Select-String 't=\d+s du_kb/dl_files/pids: (\d+)' | Select-Object -Last 1).Matches.Groups[1].Value
    $cmp = ($log | Select-String 'compare: .*differing bytes in prefix: (\d+)' | ForEach-Object { $_.Matches[0].Groups[1].Value }) -join '/'
    $line = "$(Get-Date -Format HH:mm:ss) $label unzip_errors=$err du_before_kill=$before du_after_boot=$after du_end=$last prefix_diff_bytes=[$cmp]"
    Add-Content $sum $line -Encoding utf8
    $line
    if ($null -eq $err) { 'round did not finish cleanly; stopping'; break }
    if ($err -gt 0) { 'unzip error reproduced; keeping evidence and stopping'; break }
    & $Adb -P $AdbPort -s $Serial shell "rm -rf /data/local/tmp/t96/crash-$label /data/media/0/t96grab-$label" 2>&1 | Out-Null
}