# 回归项：《明日方舟：终末地》冷启动到标题的秒数，新版和对照版交替各 Rounds 轮（新、旧、新、旧……）。
# 用 coldprof.ps1（终末地专用的冷启动采样，看到标题参考图就停）。宿主忙的时候测出来的不算数（mas-35 回归时
# 111 s 那次就是宿主在换页），所以每轮开跑前等到可用内存 ≥ QuietMB、且没有发版的 7z / release_tools.py 在跑。
# 通过：新版到标题秒数的中位数不比对照版慢过 max(10 s, 15%)，且每轮都到了标题。
param(
    [Parameter(Mandatory)][string]$Sdk,
    [Parameter(Mandatory)][string]$Baseline,
    # 默认 <TempRoot>\regress-ef
    [string]$Out,
    [int]$Rounds = 2,
    [int]$QuietMB = 22000,
    [double]$MaxWaitHours = 4,
    # 终末地冷启动采样脚本 coldprof.ps1，仓库外（本机配置 Regress.Coldprof）
    [string]$Coldprof
)
$ErrorActionPreference = 'Stop'
[cultureinfo]::CurrentCulture = [cultureinfo]::InvariantCulture
. "$PSScriptRoot\common.ps1"
if (-not $Out) { $Out = Join-Path (Use-Local TempRoot $null '-Out') 'regress-ef' }
$Coldprof = Use-Local Regress.Coldprof $Coldprof '-Coldprof'
New-Item -ItemType Directory -Force $Out | Out-Null

function Quiet {
    $avail = (Get-Counter '\Memory\Available MBytes').CounterSamples[0].CookedValue
    $busy = @(Get-CimInstance Win32_Process -Filter "Name='7z.exe' OR Name='python.exe'" | Where-Object { $_.Name -eq '7z.exe' -or $_.CommandLine -match 'release_tools\.py' }).Count
    [pscustomobject]@{ Ok = ($avail -ge $QuietMB -and $busy -eq 0); Avail = $avail; Busy = $busy }
}
function WaitQuiet([string]$name) {
    $t0 = Get-Date; $said = $false
    while ($true) {
        $q = Quiet
        if ($q.Ok) { Say ("{0}：可用 {1:N0} MB" -f $name, $q.Avail); return $true }
        if (-not $said) { Say ("{0}：等宿主空下来（可用 {1:N0} MB，发版进程 {2} 个）" -f $name, $q.Avail, $q.Busy); $said = $true }
        if (((Get-Date) - $t0).TotalHours -ge $MaxWaitHours) { return $false }
        Start-Sleep 60
    }
}
function Median([double[]]$v) { $s = $v | Sort-Object; if (-not $s) { return $null }; $n = $s.Count; if ($n % 2) { $s[[int][math]::Floor($n / 2)] } else { ($s[$n / 2 - 1] + $s[$n / 2]) / 2 } }

$labelNew = (Split-Path $Sdk -Leaf) -replace '^sdk-', ''
$labelOld = (Split-Path $Baseline -Leaf) -replace '^sdk-', ''
$times = @{ $labelNew = @(); $labelOld = @() }
$problems = @()
for ($k = 1; $k -le $Rounds * 2; $k++) {
    $isNew = ($k % 2) -eq 1
    $sdkK = if ($isNew) { $Sdk } else { $Baseline }
    $label = if ($isNew) { $labelNew } else { $labelOld }
    if (-not (WaitQuiet "第 $k 轮 $label")) { $problems += "第 $k 轮：$MaxWaitHours 小时内宿主一直不空"; break }
    $o = Join-Path $Out "ef-$label-$k.console.txt"
    $p = Start-Process pwsh -ArgumentList @('-NoProfile', '-File', $Coldprof, 'session', '-Game', 'ef', '-Mode', 'WT', '-Runs', 'cold',
        '-AfterTitle', '3', '-Seconds', '180', '-Sdk', $sdkK, '-Label', "regress-$label-$k") -RedirectStandardOutput $o -RedirectStandardError "$o.err" -WindowStyle Hidden -PassThru
    if (-not $p.WaitForExit(20 * 60 * 1000)) { $problems += "第 $k 轮 20 分钟没结束（进程 $($p.Id) 留着）"; break }
    $t = Select-String -LiteralPath $o -Pattern 'r1-cold:t=(\d+)' | Select-Object -Last 1
    if ($t) { $sec = [int]$t.Matches[0].Groups[1].Value; $times[$label] += $sec; Say "第 $k 轮 $label：到标题 $sec 秒" }
    else { $problems += "第 $k 轮 $label 没到标题（见 $o）" }
}
$mNew = Median $times[$labelNew]; $mOld = Median $times[$labelOld]
$summary = "$labelNew 到标题 $(($times[$labelNew]) -join ' / ') 秒，$labelOld $(($times[$labelOld]) -join ' / ') 秒"
if ($null -ne $mNew -and $null -ne $mOld -and $mNew -gt $mOld + [math]::Max(10, 0.15 * $mOld)) { $problems += "$labelNew 中位数 $mNew 秒比 $labelOld 的 $mOld 秒慢" }
if ($problems) { $summary += "；问题：$($problems -join '；')" }
Result ($problems.Count -eq 0) $summary
exit 0
