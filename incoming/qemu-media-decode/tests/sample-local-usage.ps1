# Copyright 2026 jinghero. SPDX-License-Identifier: Apache-2.0
param(
    [Parameter(Mandatory)][string]$Adb,
    [Parameter(Mandatory)][int]$ServerPort,
    [Parameter(Mandatory)][string]$Serial,
    [Parameter(Mandatory)][int]$QemuPid,
    [Parameter(Mandatory)][string]$Snapshot,
    [Parameter(Mandatory)][string]$Csv
)
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $Csv) { throw 'Choose a new CSV for each run.' }
$qemu = Get-Process -Id $QemuPid
if ($qemu.ProcessName -notlike 'qemu-system-*') { throw 'Expected the test AVD QEMU PID.' }
for ($load = 0; $load -le 10; $load++) {
    if ($qemu.HasExited) { throw 'Test QEMU exited.' }
    $other = Get-Process -Name 'qemu-system-*' -ErrorAction SilentlyContinue | Where-Object Id -ne $QemuPid
    if ($other) { throw "Another QEMU is active. Discard the partial run at load $load." }
    if ($load) {
        $result = & $Adb -P $ServerPort -s $Serial emu avd snapshot load $Snapshot
        if ($LASTEXITCODE -ne 0 -or ($result -join ' ').Trim() -ne 'OK') {
            throw "Snapshot load $load failed: $result"
        }
    }
    Start-Sleep -Seconds 3
    for ($sample = 1; $sample -le 3; $sample++) {
        $rows = @(Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory |
                Where-Object Name -like "pid_${QemuPid}_*")
        if ($rows.Count -ne 1) { throw 'Expected one GPU adapter for the test QEMU.' }
        [pscustomobject]@{
            Load = $load
            Sample = $sample
            Instance = $rows[0].Name
            LocalBytes = [uint64]$rows[0].LocalUsage
        } | Export-Csv -LiteralPath $Csv -NoTypeInformation -Append
        if ($sample -lt 3) { Start-Sleep -Seconds 1 }
    }
}
