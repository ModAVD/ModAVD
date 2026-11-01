# 各项脚本共用（. "$PSScriptRoot\common.ps1"）：测试根换 SDK、调 modavd、私有 adb、结果行、本机配置（tools\local.ps1）。
# 开关机一律走测试根自己的 tools\modavd.exe：开机后的设置和产品一样，关机先正常关机、不成再 sync 后结束。
. (Join-Path $PSScriptRoot '..\..\local.ps1')

# 管理器和随包脚本：regress.ps1 的 -Manager / -PackageScripts 经环境变量传下来；单独跑某一项时取本机配置的
# Regress.Manager（放着 tools\modavd.exe 和 ModAVD.exe 的目录），随包脚本取本仓库的 package\。
$script:ManagerDir = Use-Local Regress.Manager $env:MODAVD_REGRESS_MANAGER '-Manager（regress.ps1）' -Optional
$script:PackageScripts = if ($env:MODAVD_REGRESS_PACKAGE) { $env:MODAVD_REGRESS_PACKAGE } else { [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\package')) }

# 测试根的 tools\modavd.exe、ModAVD.exe、tools\modavd-bridge.ps1 换成上面这一版的：测试根是很早以前建的，不同步就一直在测
# 旧的命令行。内容一样就不动。
function Sync-RootTools([string]$Root) {
    # 一行一对（前面的逗号让每对保持成一个数组，不被拆开）
    $pairs = @(
        if ($script:ManagerDir) {
            , @((Join-Path $script:ManagerDir 'tools\modavd.exe'), (Join-Path $Root 'tools\modavd.exe'))
            , @((Join-Path $script:ManagerDir 'ModAVD.exe'), (Join-Path $Root 'ModAVD.exe'))
        }
        , @((Join-Path $script:PackageScripts 'modavd-bridge.ps1'), (Join-Path $Root 'tools\modavd-bridge.ps1')))
    foreach ($p in $pairs) {
        if (-not (Test-Path -LiteralPath $p[0])) { continue }
        if ((Test-Path -LiteralPath $p[1]) -and (Get-FileHash -LiteralPath $p[0]).Hash -eq (Get-FileHash -LiteralPath $p[1]).Hash) { continue }
        New-Item -ItemType Directory -Force (Split-Path $p[1]) | Out-Null
        Copy-Item -LiteralPath $p[0] -Destination $p[1] -Force
    }
}

# 把测试根 mas-avd.json 的 sdkRoot 换成 $Sdk，返回原值（跑完换回去）。只改这一个值，其余字节不动。顺带同步测试根的工具。
function Set-RootSdk([string]$Root, [string]$Sdk) {
    Sync-RootTools $Root
    $p = Join-Path $Root 'mas-avd.json'
    $t = [IO.File]::ReadAllText($p)
    $m = [regex]::Match($t, '"sdkRoot":\s*"((?:[^"\\]|\\.)*)"')
    if (-not $m.Success) { throw "$p 里没有 sdkRoot" }
    $g = $m.Groups[1]
    $new = $t.Substring(0, $g.Index) + ($Sdk -replace '\\', '\\') + $t.Substring($g.Index + $g.Length)
    [IO.File]::WriteAllText($p, $new, [Text.UTF8Encoding]::new($false))
    return ($g.Value -replace '\\\\', '\')
}

function Get-RootConfig([string]$Root) { Get-Content -LiteralPath (Join-Path $Root 'mas-avd.json') -Raw | ConvertFrom-Json }

# modavd <参数...>，输出逐行返回，退出码在 $LASTEXITCODE
function M { & (Join-Path $script:Root 'tools\modavd.exe') --root $script:Root @args 2>&1 | ForEach-Object { "$_" } }

# 对第 $script:Index 台实例的 adb（测试根的私有 adb server 和同一个 SDK 的 adb）
function A {
    $cfg = Get-RootConfig $script:Root
    if ($cfg.adbServerPort -in 5037, 20049, 20050) { throw "测试根的 adb 端口是 $($cfg.adbServerPort)，不碰生产端口" }
    & (Join-Path $cfg.sdkRoot 'platform-tools\adb.exe') -P $cfg.adbServerPort -s "emulator-$(20000 + 10 * $script:Index)" @args 2>&1 | ForEach-Object { "$_" }
}

# 以 root 跑一段客体脚本：推成文件再 sh，免得引号穿过 adb shell / su 两层
function Su([string]$Script) {
    $tmp = Join-Path ([IO.Path]::GetTempPath()) "regress-$PID.sh"
    [IO.File]::WriteAllText($tmp, ($Script -replace "`r`n", "`n"))
    A push $tmp /data/local/tmp/regress.sh | Out-Null
    Remove-Item -LiteralPath $tmp -Force
    A shell 'su 0 sh /data/local/tmp/regress.sh; rm -f /data/local/tmp/regress.sh'
}

function Say([string]$m) { Write-Output "$(Get-Date -Format 'HH:mm:ss') $m" }

# 删掉本项自己新建的测试实例。刚关机时 modavd 的开机看护进程可能还拿着启动锁（「正在启动，无法删除」），重试最多 60 秒。
function Remove-TestInstance([int]$i) {
    for ($k = 0; $k -lt 30; $k++) {
        $o = M delete $i --yes
        if ($LASTEXITCODE -eq 0) { $o | Select-Object -Last 1 | ForEach-Object { Say "  $_" }; return }
        Start-Sleep 2
    }
    Say "  实例 $i 没删掉：$(($o | Select-Object -Last 1))"
}

# 运行器认的结果行（最后一行为准）
function Result([bool]$Pass, [string]$Summary) {
    'REGRESS-RESULT ' + ([ordered]@{ pass = $Pass; summary = $Summary } | ConvertTo-Json -Compress)
}
