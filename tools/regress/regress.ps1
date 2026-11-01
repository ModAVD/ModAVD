#requires -Version 7.0
<#
发版前回归的 e2e 运行器：按套件文件（suites\*.psd1）一项一项地跑，每项开跑前等宿主资源、避开不能跑的时段，
子进程跑、限时，按每项的判据给出「通过 / 不通过」，最后写出 results.json 和可以直接贴进
tools\pubrel\config\changes.md「发布前回归」的表格 results.md。

    pwsh -NoProfile -File tools\regress\regress.ps1 -Suite tools\regress\suites\release.psd1 -Scope emulator -Sdk <新 SDK> -Baseline <上一个发布版的 SDK>
    pwsh -NoProfile -File tools\regress\regress.ps1 -Suite ... -Scope manager,ime -Sdk ...      # 改了什么跑什么（10-30 定）
    pwsh -NoProfile -File tools\regress\regress.ps1 -Suite ... -Sdk ... -Only bench,sr          # 只跑这几项
    pwsh -NoProfile -File tools\regress\regress.ps1 -Suite ... -Scope emulator -Sdk ... -DryRun  # 只列出会跑什么

-Scope 取套件 Scopes 里的名字（可以几个），跑 Scopes 和它有交集的项；套件有 Scopes 时必须给 -Scope 或 -Only。

单线程：一次只跑一项，一项一台模拟器（和生产抢不到多少内存，也不会两项抢同一台实例）。
某项超时就停在这里，后面的不跑：超时的那项可能还占着实例，接着跑会撞上。

每项的约定见 README.md：脚本最后打印一行 `REGRESS-RESULT {"pass":true,"summary":"..."}` 最好；
老脚本没有这一行，就在套件里给退出码、必须出现 / 不能出现的正则和摘要正则。
测试机、外部脚本这些本机的东西在本机配置的 Regress 一节（tools\local.example.psd1），套件里用 {Local:<键名>} 引用。
#>
param(
    [Parameter(Mandatory)][string]$Suite,
    [Parameter(Mandatory)][string]$Sdk,
    # A/B 项的对照 SDK（通常是上一个发布版）
    [string]$Baseline,
    # 默认取 SDK 的 Pkg.BuildId（mas-36 -> mas36）
    [string]$Label,
    # 默认 <TempRoot>\regress-<Label>-<时间>；本机配置有 TempRoot 时必须在它下面
    [string]$Out,
    # 这一版改了什么（套件 Scopes 里的名字）
    [string[]]$Scope,
    [string[]]$Only,
    [string[]]$Skip,
    # 管理器：放着 tools\modavd.exe 和 ModAVD.exe 的目录（ModAVDManager 的 build.ps1 -Out，或解开的 Release 包；
    # 本机配置 Regress.Manager）；发版回归用要随包发的那一版。随包脚本：默认本仓库的 package\，发版回归用发版克隆里的那份。
    # 两个都经环境变量（MODAVD_REGRESS_MANAGER / MODAVD_REGRESS_PACKAGE）传给各项，见 items\common.ps1
    [string]$Manager,
    [string]$PackageScripts,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
[cultureinfo]::CurrentCulture = [cultureinfo]::InvariantCulture
. (Join-Path $PSScriptRoot '..\local.ps1')
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Manager = (Resolve-Path (Use-Local Regress.Manager $Manager '-Manager')).Path
$PackageScripts = (Resolve-Path $(if ($PackageScripts) { $PackageScripts } else { Join-Path $Repo 'package' })).Path
foreach ($f in (Join-Path $Manager 'tools\modavd.exe'), (Join-Path $Manager 'ModAVD.exe'), (Join-Path $PackageScripts 'modavd-bridge.ps1')) {
    if (-not (Test-Path -LiteralPath $f)) { throw "没有 $f（-Manager / -PackageScripts）" }
}
$env:MODAVD_REGRESS_MANAGER = $Manager
$env:MODAVD_REGRESS_PACKAGE = $PackageScripts
# 记下测的是哪一版：命令行的哈希，随包脚本所在仓库的提交（有改动另注）
$cliSha = (Get-FileHash -LiteralPath (Join-Path $Manager 'tools\modavd.exe')).Hash.ToLower().Substring(0, 12)
$pkgRev = git -C $PackageScripts log -1 --format=%h 2>$null
$pkgRev = if (-not $pkgRev) { '不在 git 仓库里' } elseif (git -C $PackageScripts status --porcelain -- . 2>$null) { "$pkgRev（有未提交的改动）" } else { $pkgRev }
$public = "管理器 $Manager（tools\modavd.exe $cliSha）；随包脚本 $PackageScripts（$pkgRev）"
$Sdk = (Resolve-Path $Sdk).Path
if (-not (Test-Path "$Sdk\emulator\emulator.exe")) { throw "不像 SDK 根目录：$Sdk" }
if ($Baseline) { $Baseline = (Resolve-Path $Baseline).Path }
if (-not $Label) {
    $id = (Select-String -Path "$Sdk\emulator\source.properties" -Pattern '^Pkg\.BuildId=(.+)$').Matches[0].Groups[1].Value.Trim()
    $Label = $id -replace '-', ''
}
$suiteData = Import-PowerShellDataFile -LiteralPath $Suite
# 临时数据只放临时目录：本机配置有 TempRoot 时输出必须在它下面
$tempRoot = Use-Local TempRoot $null '' -Optional
if (-not $Out) { $Out = Join-Path (Use-Local TempRoot $null '-Out') "regress-$Label-$(Get-Date -Format 'yyyyMMdd-HHmmss')" }
$Out = [IO.Path]::GetFullPath($Out)
if ($tempRoot -and -not (Test-Inside $Out @($tempRoot))) { throw "输出要在临时目录 $tempRoot（本机配置 TempRoot）下面：$Out" }

$items = @($suiteData.Items)
$ids = $items | ForEach-Object { $_.Id }
# pwsh -File 把 `-Only a,b` 当成一个字符串
$Only = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$Skip = @($Skip | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
foreach ($x in @($Only) + @($Skip)) { if ($x -and $ids -notcontains $x) { throw "套件里没有这一项：$x（有：$($ids -join ', ')）" } }
$Scope = @($Scope | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$scopeNames = if ($suiteData.Scopes) { @($suiteData.Scopes.Keys) } else { @() }
foreach ($x in $Scope) { if ($scopeNames -notcontains $x) { throw "套件里没有这个改动范围：$x（有：$($scopeNames -join ', ')）" } }
if ($scopeNames.Count -and -not $Scope -and -not $Only) {
    throw "要给 -Scope（这一版改了什么）或 -Only：$(($suiteData.Scopes.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name)＝$($_.Value)" }) -join '；')"
}
if ($Scope) { $items = @($items | Where-Object { @($_.Scopes | Where-Object { $Scope -contains $_ }).Count -gt 0 }) }
if ($Only) { $items = @($items | Where-Object { $Only -contains $_.Id }) }
if ($Skip) { $items = @($items | Where-Object { $Skip -notcontains $_.Id }) }

# {Local:<键名>} 取本机配置 Regress 一节；没配的换成 <没配置 Regress.键名>，记进 $script:unset，那一项不跑
$script:unset = [System.Collections.Generic.List[string]]::new()
function Expand([string]$s, [string]$itemOut) {
    $s = $s.Replace('{Sdk}', $Sdk).Replace('{Baseline}', [string]$Baseline).Replace('{Label}', $Label).Replace('{Out}', $Out).
    Replace('{ItemOut}', $itemOut).Replace('{Repo}', $Repo)
    [regex]::Replace($s, '\{Local:([A-Za-z0-9]+)\}', {
            param($m)
            $v = Use-Local "Regress.$($m.Groups[1].Value)" $null '' -Optional
            if ($v) { return [string]$v }
            $script:unset.Add("Regress.$($m.Groups[1].Value)")
            "<没配置 Regress.$($m.Groups[1].Value)>"
        })
}

# 打完包还要用组包目录补跑的（套件 PostPackage，按改动范围挑）
$postPackage = @(if ($Scope -and $suiteData.PostPackage) {
        $suiteData.PostPackage.GetEnumerator() | Where-Object { @($_.Value | Where-Object { $Scope -contains $_ }).Count -gt 0 } | ForEach-Object { $_.Name } | Sort-Object
    })

if ($DryRun) {
    "套件 $Suite：$($items.Count) 项；改动范围 $(if ($Scope) { $Scope -join ', ' } else { '（按 -Only 点名）' })；SDK $Sdk（$Label）；对照 $(if ($Baseline) { $Baseline } else { '无' })；输出 $Out"
    $public
    foreach ($it in $items) {
        $a = @($it.Args) | ForEach-Object { Expand $_ "$Out\$($it.Id)" }
        "  [$($it.Id)] $($it.Title)`n      $(Expand $it.Script "$Out\$($it.Id)") $($a -join ' ')"
    }
    if ($postPackage) { "打完包还要用组包目录跑：$($postPackage -join '；')" }
    if ($script:unset.Count) { "本机配置里没有、用到它的项不会跑：$(($script:unset | Sort-Object -Unique) -join '、')" }
    return
}
New-Item -ItemType Directory -Force $Out | Out-Null
$log = Join-Path $Out 'regress.log'
function L([string]$m) { $l = "$(Get-Date -Format 'HH:mm:ss') $m"; Add-Content -LiteralPath $log -Value $l -Encoding utf8; Write-Host $l }

# 生产和别的会话也在用这台机器：每项开跑前等空闲物理内存和提交余量都够，每分钟查一次
function WaitRes($it) {
    $needFree = if ($null -ne $it.NeedFreeGB) { $it.NeedFreeGB } else { 8 }
    $needCommit = if ($null -ne $it.NeedCommitGB) { $it.NeedCommitGB } else { 12 }
    $maxHours = if ($null -ne $it.MaxWaitHours) { $it.MaxWaitHours } else { 3 }
    $t0 = Get-Date; $said = $false
    while ($true) {
        $os = Get-CimInstance Win32_OperatingSystem
        $free = $os.FreePhysicalMemory / 1MB; $commit = $os.FreeVirtualMemory / 1MB
        if ($free -ge $needFree -and $commit -ge $needCommit) {
            L ("[{0}] 空闲内存 {1:N1} GB，提交余量 {2:N1} GB{3}" -f $it.Id, $free, $commit, $(if ($said) { "，等了 $([int]((Get-Date) - $t0).TotalMinutes) 分钟" } else { '' }))
            return $true
        }
        if (-not $said) { L ("[{0}] 等资源：空闲 {1:N1} / {2} GB，提交余量 {3:N1} / {4} GB" -f $it.Id, $free, $needFree, $commit, $needCommit); $said = $true }
        if (((Get-Date) - $t0).TotalHours -ge $maxHours) { return $false }
        Start-Sleep 60
    }
}

# 不能跑的时段（例如生产在用同一个游戏号），'HH:mm-HH:mm'
function InAvoid($it) {
    $now = (Get-Date).TimeOfDay
    foreach ($w in @($it.Avoid)) {
        if (-not $w) { continue }
        $a, $b = $w -split '-'
        if ($now -ge [timespan]$a -and $now -lt [timespan]$b) { return $w }
    }
    return $null
}

# 子进程里先把输出编码设成 UTF-8（不然重定向出来的中文是问号）。命令行原样拼出来：`-名字` 照写（参数名、开关），
# 其余一律单引号字面量（数组展开传参时 `-名字` 只会被当成位置参数的值）。
# 退出码：脚本 exit 的值；脚本没 exit、却出了错（含参数绑定失败）就是 1。
function Start-Item([string]$script, [string[]]$argv, [string]$console) {
    $line = ($argv | ForEach-Object { if ($_ -match '^-[A-Za-z][A-Za-z0-9]*$') { $_ } else { "'" + ($_ -replace "'", "''") + "'" } }) -join ' '
    $cmd = "[Console]::OutputEncoding = [Text.UTF8Encoding]::new(`$false); `$OutputEncoding = [Console]::OutputEncoding; " +
        "`$global:LASTEXITCODE = 0; & '$($script -replace "'", "''")' $line; `$ok = `$?; " +
        "if (`$LASTEXITCODE) { exit `$LASTEXITCODE }; exit ([int](-not `$ok))"
    $b64 = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($cmd))
    Start-Process pwsh -ArgumentList @('-NoProfile', '-NonInteractive', '-OutputFormat', 'Text', '-EncodedCommand', $b64) `
        -RedirectStandardOutput $console -RedirectStandardError "$console.err" -WindowStyle Hidden -PassThru
}

function Verdict($it, [string]$text, [int]$exit) {
    $why = @()
    # 'any'：退出码不算数，只看正则（体检在宿主没开测速服务时 net 报 ERROR、退出码 1）
    if ($it.ExitCode -ne 'any') {
        $wantExit = if ($null -ne $it.ExitCode) { [int]$it.ExitCode } else { 0 }
        if ($exit -ne $wantExit) { $why += "退出码 $exit" }
    }
    $m = [regex]::Matches($text, '(?m)^.*?REGRESS-RESULT (\{.*\})\s*$')
    if ($m.Count) {
        $j = $m[$m.Count - 1].Groups[1].Value | ConvertFrom-Json
        if (-not $j.pass) { $why += '项目自己判不通过' }
        return [pscustomobject]@{ Pass = ($why.Count -eq 0); Summary = [string]$j.summary; Why = $why }
    }
    # 既没有结果行、也没有必须出现的正则：判不了，不能当通过（脚本没跑起来也是这个样子）
    if (-not @($it.Pass | Where-Object { $_ })) { $why += '输出里没有 REGRESS-RESULT 结果行' }
    foreach ($r in @($it.Pass)) { if ($r -and $text -notmatch $r) { $why += "没有「$r」" } }
    foreach ($r in @($it.Fail)) { if ($r -and $text -match $r) { $why += "出现「$($Matches[0].Trim())」" } }
    $summary = ''
    if ($it.Summary) {
        $sm = [regex]::Matches($text, $it.Summary)
        if ($sm.Count) { $g = $sm[$sm.Count - 1]; $summary = if ($g.Groups.Count -gt 1) { (@($g.Groups | Select-Object -Skip 1 | ForEach-Object { $_.Value }) -join ' ').Trim() } else { $g.Value.Trim() } }
    }
    if (-not $summary) { $summary = (($text -split "`r?`n") | Where-Object { $_.Trim() } | Select-Object -Last 1) }
    [pscustomobject]@{ Pass = ($why.Count -eq 0); Summary = $summary; Why = $why }
}

$results = [System.Collections.Generic.List[object]]::new()
$sdkId = (Select-String -Path "$Sdk\emulator\source.properties" -Pattern '^Pkg\.BuildId=').Line
$outDrive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($Out))
L "开始：套件 $Suite（$($items.Count) 项，改动范围 $(if ($Scope) { $Scope -join ', ' } else { '按 -Only 点名' })）；SDK $Sdk（$sdkId）；对照 $(if ($Baseline) { $Baseline } else { '无' })；输出所在的 $($outDrive.Name) 可用 $([int]($outDrive.AvailableFreeSpace / 1GB)) GB"
L $public
$stopped = $false
foreach ($it in $items) {
    $r = [ordered]@{ id = $it.Id; title = $it.Title; criterion = $it.Criterion; status = '未跑'; summary = ''; seconds = 0; gpu_events = 0; console = '' }
    if ($stopped) { $results.Add([pscustomobject]$r); continue }
    if ($it.NeedsBaseline -and -not $Baseline) { $r.status = '跳过'; $r.summary = '没给 -Baseline'; L "[$($it.Id)] 跳过：没给 -Baseline"; $results.Add([pscustomobject]$r); continue }
    $itemOut = Join-Path $Out $it.Id
    $script:unset.Clear()
    $script = Expand $it.Script $itemOut
    $argv = @($it.Args) | Where-Object { $null -ne $_ } | ForEach-Object { Expand $_ $itemOut }
    if ($script:unset.Count) {
        $r.status = '跳过'; $r.summary = "本机配置里没有 $(($script:unset | Sort-Object -Unique) -join '、')"
        L "[$($it.Id)] 跳过：$($r.summary)"; $results.Add([pscustomobject]$r); continue
    }
    $w = InAvoid $it
    if ($w) { $r.status = '跳过'; $r.summary = "在不能跑的时段 $w"; L "[$($it.Id)] 跳过：现在在 $w 里"; $results.Add([pscustomobject]$r); continue }
    if (-not (WaitRes $it)) { $r.status = '跳过'; $r.summary = '资源一直不够'; L "[$($it.Id)] 跳过：资源一直不够"; $results.Add([pscustomobject]$r); continue }
    New-Item -ItemType Directory -Force $itemOut | Out-Null
    $console = Join-Path $Out "$($it.Id).console.txt"
    $r.console = $console
    $timeout = if ($it.TimeoutMin) { $it.TimeoutMin } else { 40 }
    L "[$($it.Id)] 开跑：$script $($argv -join ' ')"
    $t0 = Get-Date
    $p = Start-Item $script $argv $console
    $done = $p.WaitForExit([int]($timeout * 60 * 1000))
    $r.seconds = [int]((Get-Date) - $t0).TotalSeconds
    # 显卡驱动重置（nvlddmkm）记在系统日志里；跑期间有就记下来，判据里要不要算由人看
    $r.gpu_events = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'nvlddmkm'; StartTime = $t0 } -ErrorAction SilentlyContinue).Count
    if (-not $done) {
        $r.status = '超时'; $r.summary = "$timeout 分钟没结束（进程 $($p.Id) 留着，后面的项不跑了）"
        L "[$($it.Id)] 超时：$($r.summary)"; $stopped = $true; $results.Add([pscustomobject]$r); continue
    }
    $p.WaitForExit()
    $text = (Get-Content -LiteralPath $console -Raw -Encoding utf8 -ErrorAction SilentlyContinue) + "`n" +
        (Get-Content -LiteralPath "$console.err" -Raw -Encoding utf8 -ErrorAction SilentlyContinue)
    $v = Verdict $it $text $p.ExitCode
    $r.status = if ($v.Pass) { '通过' } else { '不通过' }
    $r.summary = $v.Summary
    if ($v.Why) { $r.why = $v.Why -join '；' }
    L ("[{0}] {1}（{2} s，退出码 {3}，显卡事件 {4}）：{5}{6}" -f $it.Id, $r.status, $r.seconds, $p.ExitCode, $r.gpu_events, $r.summary,
        $(if ($v.Why) { "；原因：$($v.Why -join '；')" } else { '' }))
    $results.Add([pscustomobject]$r)
}

# 测试机不该留着：套件里列的 AVD 名字还有 qemu 在跑就记下来
$names = @($suiteData.Avds)
$left = @(Get-CimInstance Win32_Process -Filter "Name like 'qemu-system%'" | Where-Object { $c = $_.CommandLine; $names | Where-Object { $c -match "-avd\s+$([regex]::Escape($_))(\s|$)" } })
L "留下的测试机 qemu：$(if ($left) { ($left | ForEach-Object { $_.ProcessId }) -join ',' } else { '无' })"

$results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $Out 'results.json') -Encoding utf8
$md = @('| 项目 | 判据 | 结果 |', '|---|---|---|')
foreach ($r in $results) {
    $cell = switch ($r.status) { '通过' { "通过：$($r.summary)" } '不通过' { "**不通过**：$($r.summary)$(if ($r.why) { "（$($r.why)）" })" } default { "$($r.status)：$($r.summary)" } }
    $md += "| $($r.title) | $($r.criterion) | $($cell -replace '\|', '/' -replace '\r?\n', ' ') |"
}
$md | Set-Content -LiteralPath (Join-Path $Out 'results.md') -Encoding utf8
$bad = @($results | Where-Object { $_.status -ne '通过' })
L "全部结束：$($results.Count) 项，通过 $($results.Count - $bad.Count)，其余 $($bad.Count)（$(($bad | ForEach-Object { "$($_.id)=$($_.status)" }) -join ', ')）；表格 $(Join-Path $Out 'results.md')"
if ($postPackage) { L "打完包还要用组包目录跑：$($postPackage -join '；')" }
exit ([int]($bad.Count -gt 0))
