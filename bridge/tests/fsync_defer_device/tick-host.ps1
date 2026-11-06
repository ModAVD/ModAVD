# Copyright 2026 qiyinxi. SPDX-License-Identifier: AGPL-3.0-or-later.
# Run tick in the guest over adb and timestamp each line on the host (Stopwatch). Writes <out>.csv:
# host_ms;guest_mono_ms;guest_real_ms. Then prints every gap > 250 ms seen by the host or the guest.
# (tick must already be pushed to /data/local/tmp/t96/tick, see build.ps1 / README.)
param([int]$Secs = 120, [Parameter(Mandatory)][string]$Out, [Parameter(Mandatory)][int]$AdbPort, [Parameter(Mandatory)][string]$Serial, [string]$Adb = 'adb')
$psi = [Diagnostics.ProcessStartInfo]::new($Adb, "-P $AdbPort -s $Serial shell /data/local/tmp/t96/tick $Secs")
$psi.RedirectStandardOutput = $true
$psi.UseShellExecute = $false
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = [Diagnostics.Process]::Start($psi)
$rows = [Collections.Generic.List[string]]::new()
while ($null -ne ($line = $p.StandardOutput.ReadLine())) {
    $rows.Add("$($sw.ElapsedMilliseconds);$($line.Trim() -replace ' ', ';')")
}
$p.WaitForExit()
Set-Content "$Out.csv" $rows -Encoding utf8
$prev = $null
$hostGaps = 0; $guestGaps = 0
foreach ($r in $rows) {
    $v = $r -split ';'
    if ($v.Count -lt 3) { continue }
    $h = [long]$v[0]; $g = [long]$v[1]
    if ($prev) {
        $dh = $h - $prev[0]; $dg = $g - $prev[1]
        if ($dh -gt 250 -or $dg -gt 250) {
            "host_ms={0,7} host gap {1,5} ms | guest monotonic gap {2,5} ms" -f $h, $dh, $dg
            if ($dh -gt 250) { $hostGaps++ }; if ($dg -gt 250) { $guestGaps++ }
        }
    }
    $prev = @($h, $g)
}
$first = $rows[0] -split ';'; $last = $rows[$rows.Count - 1] -split ';'
"lines=$($rows.Count) host elapsed=$([long]$last[0] - [long]$first[0]) ms guest elapsed=$([long]$last[1] - [long]$first[1]) ms; gaps>250ms host=$hostGaps guest=$guestGaps"
