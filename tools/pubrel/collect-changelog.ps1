#requires -Version 7.0
# 把上一版以后各仓库 changelog.d 里新加的说明文件汇总成 config\changelog.json 最前面本版那一条（公告的「更新内容」）。
# 组件仓库的路径和 release.ps1 一样取参数或本机配置；先在各仓库里 git fetch upstream --tags。说明见 README.md「发版」。
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^mas[1-9][0-9]*$')][string]$Build,
    [Parameter(Mandatory)][ValidatePattern('^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$')][string]$Version,
    # ModAVD release checkout (default: the checkout these tools are in); read at its HEAD
    [string]$Repository,
    # component checkouts, read at the head of upstream's release/<X>.<Y>; one without that branch adds nothing
    [string]$ManagerRepo,
    [string]$ExtrasRepo,
    [string]$ImeRepo,
    [string]$Changelog = (Join-Path $PSScriptRoot 'config\changelog.json'),
    # hand-written 说明 items: what the user has to do for this version
    [string[]]$Note = @(),
    # the AUTO-MAS version this release needs ('none' = no requirement); default: the previous version's
    [string]$MinAutoMas,
    [string]$Python
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
. (Join-Path $PSScriptRoot '..\local.ps1')
$ManagerRepo = Use-Local ManagerRepo $ManagerRepo '-ManagerRepo' -Optional
$ExtrasRepo = Use-Local ExtrasRepo $ExtrasRepo '-ExtrasRepo' -Optional
$ImeRepo = Use-Local ImeRepo $ImeRepo '-ImeRepo' -Optional
if (-not $Repository) { $Repository = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
if (-not $Python) { $Python = $env:MODAVD_PYTHON }
if (-not $Python) { $Python = Use-Local Python $null '-Python' -Optional }
if (-not $Python) { $Python = (Get-Command python.exe -ErrorAction SilentlyContinue).Source }
if (-not $Python) { throw '找不到 Python 3.12：给 -Python，或者设 MODAVD_PYTHON、本机配置 Python' }

$releaseBranch = 'release/' + (($Version -split '\.')[0..1] -join '.')
$arguments = @('changelog-collect', '--build', $Build, '--version', $Version, '--changelog', $Changelog,
    '--repository', $Repository, '--release-branch', $releaseBranch)
if ($ManagerRepo) { $arguments += @('--manager-repo', $ManagerRepo) }
if ($ExtrasRepo) { $arguments += @('--extras-repo', $ExtrasRepo) }
if ($ImeRepo) { $arguments += @('--ime-repo', $ImeRepo) }
foreach ($item in $Note) { $arguments += @('--note', $item) }
if ($MinAutoMas) { $arguments += @('--min-automas', $MinAutoMas) }
& $Python -I -X utf8 (Join-Path $PSScriptRoot 'release_tools.py') @arguments
if ($LASTEXITCODE -ne 0) { throw "release_tools.py 返回 $LASTEXITCODE" }
