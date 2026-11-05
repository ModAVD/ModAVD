# Copyright 2026 Ji Mao Cai
# SPDX-License-Identifier: GPL-2.0-only
# PowerShell 7 on Windows. Lab must be new. Supply a built package for -Patched.
param(
    [Parameter(Mandatory)][string]$Package,
    [Parameter(Mandatory)][string]$Manager,
    [Parameter(Mandatory)][string]$Lab,
    [Parameter(Mandatory)][ValidateRange(21000,48000)][int]$PortBase,
    [switch]$Patched
)
$ErrorActionPreference = 'Stop'
$Package = (Resolve-Path -LiteralPath $Package).Path
$Manager = (Resolve-Path -LiteralPath $Manager).Path
$Lab = [IO.Path]::GetFullPath($Lab)
if ((Test-Path -LiteralPath $Lab) -or $Lab.StartsWith("$Package\", 'OrdinalIgnoreCase')) {
    throw 'Choose a new lab outside the source package'
}
if ($PortBase % 100 -or (Get-NetTCPConnection | Where-Object {
    $_.LocalPort -ge $PortBase -and $_.LocalPort -lt $PortBase + 100
})) { throw 'Choose a free range of 100 ports' }
New-Item -ItemType Directory -Path $Lab | Out-Null
$avd = Join-Path $Lab 'avd'
$temp = Join-Path $Lab 'temp'
$files = Join-Path $temp 'AndroidEmulator'
$userDir = Join-Path $Lab 'android-user'
New-Item -ItemType Directory -Path $files,$userDir | Out-Null
foreach ($part in 'sdk','components','guest-patches') {
    & robocopy "$Package\$part" "$Lab\$part" /E /XJ /R:1 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Copy failed for $part" }
}
$adb = "$Lab\sdk\platform-tools\adb.exe"
$emulator = "$Lab\sdk\emulator\emulator.exe"
$qemu = "$Lab\sdk\emulator\qemu\windows-x86_64\qemu-system-x86_64-headless.exe"
$server = $PortBase + 99
$serial = "emulator-$PortBase"
@{portBase=$PortBase; adbServerPort=$server; scriptAdbServerPort=$PortBase+98} |
    ConvertTo-Json | Set-Content -LiteralPath "$Lab\mas-avd.json"
$sentinels = 'TMPDEAD.tmp','TMPDEAD.tmp.qcow2','modavd-unrelated.tmp'
foreach ($name in $sentinels) { [IO.File]::WriteAllText("$files\$name", $name) }

function Run([string]$exe, [string[]]$argv, [string]$log, [switch]$Background) {
    $si = [Diagnostics.ProcessStartInfo]::new($exe)
    $si.UseShellExecute = $false
    $si.CreateNoWindow = $true
    $si.WorkingDirectory = $Lab
    $si.RedirectStandardOutput = $true
    $si.RedirectStandardError = $true
    foreach ($arg in $argv) { $si.ArgumentList.Add($arg) }
    foreach ($k in 'TEMP','TMP','TMPDIR') { $si.Environment[$k] = $temp }
    foreach ($k in 'ANDROID_USER_HOME','ANDROID_EMULATOR_HOME') { $si.Environment[$k] = $userDir }
    $si.Environment['ANDROID_AVD_HOME'] = $avd
    $si.Environment['ANDROID_SDK_ROOT'] = "$Lab\sdk"
    $si.Environment['ANDROID_HOME'] = "$Lab\sdk"
    $si.Environment['ANDROID_ADB_SERVER_PORT'] = [string]$server
    $si.Environment['ADB_SERVER_SOCKET'] = "tcp:127.0.0.1:$server"
    $si.Environment['PATH'] = "$Lab\sdk\platform-tools;" + $si.Environment['PATH']
    $p = [Diagnostics.Process]::Start($si)
    $out = [IO.File]::Create("$Lab\$log.out")
    $err = [IO.File]::Create("$Lab\$log.err")
    $run = @{p=$p; out=$out; err=$err; streams=@(
        $p.StandardOutput.BaseStream.CopyToAsync($out)
        $p.StandardError.BaseStream.CopyToAsync($err)
    )}
    if ($Background) { return $run }
    Finish $run
    if ($p.ExitCode) { throw "$log exited $($p.ExitCode)" }
}
function Finish($run) {
    if (!$run.p.WaitForExit(600000)) { throw 'Process timeout' }
    [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]$run.streams)
    $run.out.Dispose()
    $run.err.Dispose()
}
function Snapshot($phase) {
    $targets = @(Get-ChildItem -LiteralPath $files -File | Where-Object {
        $_.Name -match '^((TMP|modavd-).*|m[0-9a-z]{6})\.tmp(\.qcow2)?$'
    } | Sort-Object Name)
    $procs = @(Get-CimInstance Win32_Process | Where-Object {
        $_.ExecutablePath -and $_.ExecutablePath.StartsWith("$Lab\", 'OrdinalIgnoreCase')
    } | Select-Object Name,ProcessId,ParentProcessId,CommandLine)
    @{utc=[datetime]::UtcNow; files=@($targets | Select-Object Name,Length); processes=$procs} |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$Lab\$phase.json"
    foreach ($name in $sentinels) {
        if ([IO.File]::ReadAllText("$files\$name") -ne $name) { throw "Changed $name" }
    }
    $targets | ForEach-Object Name
}
function Device([string[]]$cmd, [string]$log) {
    Run $adb (@('-P',[string]$server,'-s',$serial) + $cmd) $log
}

function Find-Guest($launcher) {
    if ($launcher.HasExited) { return }
    $child = @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$($launcher.Id)" |
        Where-Object { $_.ExecutablePath -eq $qemu -and $_.CommandLine.Contains("$PortBase,$($PortBase+1)") })
    if ($child.Count -ne 1) { return }
    $p = Get-Process -Id $child[0].ProcessId -ErrorAction SilentlyContinue
    if ($p) {
        $null = $p.SafeHandle
        if ($p.Path -eq $qemu -and !$launcher.HasExited) { return $p }
        $p.Dispose()
    }
}

try {
    $null = Snapshot 'before-create'
    Run $Manager @('--root',$Lab,'--json','create','--index','0','--no-bridge','--cpu','2',
                   '--memory','3G','--data-gb','16','--timeout','480') 'create'
    $null = Snapshot 'after-create'
    $params = @(Get-Content -LiteralPath "$avd\mas_0.avd\emu-launch-params.txt" | Select-Object -Skip 4)
    if ($params[0] -ne '-avd' -or $params[1] -ne 'mas_0' -or
        "$PortBase,$($PortBase+1)" -notin $params) { throw 'Unexpected recorded Manager arguments' }
    $pending = @()
    foreach ($mode in 'ro-normal','ro-crash','cache-normal','cache-crash','default-after-crash','config-cache-normal','default-normal') {
        $before = @(Snapshot "$mode-before")
        $ro = $mode.StartsWith('ro')
        $cache = $mode.StartsWith('cache')
        $forceExit = $mode -in @('ro-crash','cache-crash')
        $configCache = $mode -eq 'config-cache-normal'
        $temporaryCache = $cache -or $configCache
        $configPath = "$avd\mas_0.avd\config.ini"
        $savedConfig = [IO.File]::ReadAllBytes($configPath)
        $run = $null
        $guest = $null
        try {
            if ($configCache) {
                # Manager launch arguments are unchanged. A retained non-default
                # config also leaves main.cpp without a permanent cache path.
                $configText = [Text.Encoding]::UTF8.GetString($savedConfig)
                if ($configText -notmatch '(?m)^disk\.cachePartition=yes\r?$') {
                    throw 'Expected the default cache configuration'
                }
                $configText = $configText -replace '(?m)^disk\.cachePartition=yes\r?$', 'disk.cachePartition=no'
                [IO.File]::WriteAllText($configPath, $configText, [Text.UTF8Encoding]::new($false))
            }
            Copy-Item -LiteralPath $configPath -Destination "$Lab\$mode-config.ini"
            $extra = if ($ro) {'-read-only'} elseif ($cache) {'-no-cache'}
            $run = Run $emulator (@($extra | Where-Object {$_}) + @('-verbose') + $params) $mode -Background
            $until = [datetime]::UtcNow.AddMinutes(3)
            while ($true) {
                if ($run.p.HasExited -or [datetime]::UtcNow -gt $until) { throw "$mode boot failed" }
                Start-Sleep -Seconds 2
                try { Device @('shell','getprop','sys.boot_completed') 'boot' } catch { continue }
                if (([string](Get-Content "$Lab\boot.out" -Raw)).Trim() -eq '1') { break }
            }
            $guest = Find-Guest $run.p
            if (!$guest) { throw 'Could not verify disposable child' }
            if ($configCache) {
                Copy-Item -LiteralPath "$avd\mas_0.avd\hardware-qemu.ini" -Destination "$Lab\$mode-hardware.ini"
            }
            $during = @(Snapshot "$mode-live")
            $new = @($during | Where-Object { $_ -notin $before })
            $count = if ($ro) {4} elseif ($temporaryCache) {2} else {0}
            if ($new.Count -ne $count) { throw "$mode created $($new.Count), expected $count" }
            if ($Patched -and $count -gt 0 -and @($pending | Where-Object {$_ -in $during}).Count) {
                throw 'Recovery missed a file'
            }
            if ($count -eq 0 -and @($pending | Where-Object {$_ -notin $during}).Count) {
                throw 'A launch without temporary files performed recovery'
            }
            if ($forceExit) {
                $guest.Kill()
            } else {
                Device @('shell','sync') "$mode-sync"
                Device @('emu','kill') "$mode-stop"
            }
            Finish $run
            $after = @(Snapshot "$mode-after")
            $leftNew = @($new | Where-Object { $_ -in $after })
            $expected = if ($forceExit) {$count} elseif ($temporaryCache -and !$Patched) {1} else {0}
            if ($leftNew.Count -ne $expected) { throw "$mode left $($leftNew.Count), expected $expected" }
            if ($temporaryCache -and !$Patched -and $expected -eq 1 -and !$leftNew[0].EndsWith('.qcow2')) {
                throw 'Unexpected cache remnant'
            }
            if ($count -eq 0 -and @($pending | Where-Object {$_ -notin $after}).Count) {
                throw 'A launch without temporary files removed previous remnants'
            }
            $pending = @(($pending + $new) | Where-Object {$_ -in $after} | Select-Object -Unique)
        } finally {
            if ($run -and !$run.p.HasExited) {
                # Graceful cleanup of the privately addressed VM on any failed assertion.
                try { Device @('shell','sync') 'finally-sync'; Device @('emu','kill') 'finally-stop' } catch {}
                if (!$run.p.WaitForExit(15000)) {
                    if (!$guest) { $guest = Find-Guest $run.p }
                    if (!$guest) { throw 'Could not verify test VM for cleanup' }
                    if (!$guest.HasExited) { $guest.Kill() }
                }
                Finish $run
            }
            if ($guest) { $guest.Dispose() }
            if ($configCache) { [IO.File]::WriteAllBytes($configPath, $savedConfig) }
        }
    }
} finally {
    if (Get-CimInstance Win32_Process -Filter "Name='adb.exe'" | Where-Object {
        $_.ExecutablePath -eq $adb -and $_.CommandLine -match ":$server\b"
    }) { Run $adb @('-P',[string]$server,'kill-server') 'adb-stop' }
}
