# 回归项：新建数据盘和扩容。用测试根的 modavd 新建一台 16 GB 的实例：首次开机后 inode 封顶在 1048576；
# 关机后 `set --data-gb 32`，再开机，模拟器扩大磁盘镜像、开机后在线扩文件系统，应扩满到 32 GB
# （0.36 起新建的盘；之前的模拟器建的 16 GB 盘只能到 30.4 GB）。跑完删掉这台实例（-Keep 留着）。
param(
    [Parameter(Mandatory)][string]$Sdk,
    # 测试用的 ModAVD 根目录（本机配置 Regress.TestRoot）
    [string]$Root,
    # 生产用 0、1 号；5 号的端口和脚本用的 adb server（20050）撞，modavd 不让用
    [int]$Index = 3,
    [switch]$Keep
)
$ErrorActionPreference = 'Stop'
[cultureinfo]::CurrentCulture = [cultureinfo]::InvariantCulture
. "$PSScriptRoot\common.ps1"
$Root = Use-Local Regress.TestRoot $Root '-Root'

function DataFacts {
    $o = Su 'df -i /data | tail -n 1; df -k /data | tail -n 1'
    $i = ($o[0] -split '\s+'); $k = ($o[1] -split '\s+')
    [pscustomobject]@{ Inodes = [int64]$i[1]; GB = [math]::Round([int64]$k[1] / 1MB, 1) }
}

$problems = @(); $notes = @(); $created = $false
$old = Set-RootSdk $Root $Sdk
try {
    M info $Index | Out-Null
    if ($LASTEXITCODE -eq 0) { throw "测试根里已经有实例 $Index，换 -Index 或先删掉它" }
    $o = M create --index $Index --title 回归数据盘 --memory 4G --cpu 4 --data-gb 16 --no-bridge
    if ($LASTEXITCODE -ne 0) { throw "新建失败：$($o -join ' ')" }
    $created = $true
    M start $Index | Select-Object -Last 2 | ForEach-Object { Say "  $_" }
    if ($LASTEXITCODE -ne 0) { throw "首次开机失败（modavd 退出码 $LASTEXITCODE）" }
    $f = DataFacts
    $qcow = Get-Item -LiteralPath (Join-Path $Root "avd\mas_$Index.avd\userdata-qemu.img.qcow2")
    Say "首次开机：inode $($f.Inodes)，/data $($f.GB) GB"
    $notes += "inode $($f.Inodes)"
    if ($f.Inodes -ne 1048576) { $problems += "inode 总数 $($f.Inodes)，应为 1048576" }
    M stop $Index | Select-Object -Last 1 | ForEach-Object { Say "  $_" }
    $notes += ("首次开机后 qcow2 {0:N0} MB" -f ((Get-Item -LiteralPath $qcow.FullName).Length / 1MB))

    $o = M set $Index --data-gb 32
    if ($LASTEXITCODE -ne 0) { throw "set --data-gb 32 失败：$($o -join ' ')" }
    $o = M set $Index --data-gb 16
    if ($LASTEXITCODE -eq 0) { $problems += '改小没有被拒绝' }
    $o = M start $Index
    $o | Where-Object { $_ -match '数据盘' } | ForEach-Object { Say "  $_" }
    if ($LASTEXITCODE -ne 0) { throw "扩容后开机失败（modavd 退出码 $LASTEXITCODE）" }
    $f = DataFacts
    Say "扩容后：/data $($f.GB) GB"
    $notes += "16 -> 32 GB 后 /data $($f.GB) GB"
    if ($f.GB -lt 31) { $problems += "/data 只有 $($f.GB) GB，没扩满" }
    if (-not ($o -match '数据盘已扩到')) { $problems += '开机输出里没有「数据盘已扩到」' }
} catch {
    $problems += "出错：$($_.Exception.Message)"
} finally {
    M stop $Index | Select-Object -Last 1 | ForEach-Object { Say "  $_" }
    if ($created -and -not $Keep) { Remove-TestInstance $Index }
    Set-RootSdk $Root $old | Out-Null
}
$summary = $notes -join '；'
if ($problems) { $summary += "；问题：$($problems -join '；')" }
Result ($problems.Count -eq 0) $summary
exit 0
