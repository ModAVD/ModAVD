#Requires -Version 7.3
<#
release-pr.ps1: the two machine-read parts of a ModAVD release PR (ModAVD/ModAVD#134), generated from the publish
plan (the maintainer's .psd1, the same file the local publish script reads):

  - release/<version>.issues.json in the release checkout, from the plan's Version, Manager and Issues. Commit it with
    the release PR. After the release is published, .github/workflows/close-released-issues.yml reads it on the tagged
    commit and closes the issues whose fixing PRs are in the release.
  - with -BodyFile: the "Reviewed-Head: <40 hex>" line in the release PR body file, from the plan's ReviewedHead (fill
    it in after the review, then `gh pr edit <PR> --body-file <file>`). .github/workflows/verify-release.yml checks that
    the PR's merge commit has that head's tree.

  pwsh -NoProfile -File tools/pubrel/release-pr.ps1 -PlanFile <plan.psd1> [-Repository <release checkout>] [-BodyFile <body.md>]

The plan's Issues entries are @{ Issue = 103; Prs = @('ModAVDManager#54'); Kind = 'close'; Note = '' }; Prs are
"<repository>#<number>" or "<repository>@<commit>", empty for an emulator source patch (then Note says which); Kind is
close, comment (already closed) or keep (comment, leave open). The format of issues.json is in release_ci.py.
#>
param(
    [Parameter(Mandatory)][string]$PlanFile,
    # the ModAVD checkout the release PR is made in; default: this repository
    [string]$Repository = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$BodyFile = '',
    [string]$Python = ''
)
$ErrorActionPreference = 'Stop'
if (-not $Python) { $Python = if ($env:MODAVD_PYTHON) { $env:MODAVD_PYTHON } else { (Get-Command python.exe, python3, python -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source } }
$plan = Import-PowerShellDataFile -LiteralPath $PlanFile
foreach ($k in 'Version', 'Manager', 'Issues') { if (-not $plan.ContainsKey($k)) { throw "the plan $PlanFile has no $k" } }
$version = [string]$plan.Version
if ($version -notmatch '^0\.[1-9][0-9]*\.(0|[1-9][0-9]*)$') { throw "the plan's Version is not 0.N.P: $version" }
$tool = Join-Path $PSScriptRoot 'release_ci.py'

$items = @(foreach ($e in @($plan.Issues)) {
        [ordered]@{ issue = [int]$e.Issue; prs = [string[]]@($e.Prs | Where-Object { $_ }); kind = [string]$e.Kind; note = [string]$e.Note }
    })
$tmp = Join-Path ([IO.Path]::GetTempPath()) "release-pr-$PID-$([guid]::NewGuid().ToString('N')).json"
try {
    [IO.File]::WriteAllText($tmp, (ConvertTo-Json -InputObject $items -Depth 4), [Text.UTF8Encoding]::new($false))
    $out = Join-Path $Repository "release\$version.issues.json"
    & $Python -I -B -X utf8 $tool issues-json --version $version --manager ([string]$plan.Manager) --input $tmp --output $out
    if ($LASTEXITCODE) { throw "release_ci.py issues-json failed ($LASTEXITCODE)" }
} finally { Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue }

if ($BodyFile) {
    $head = [string]$plan.ReviewedHead
    if ($head -notmatch '^[0-9a-f]{40}$') { throw "the plan's ReviewedHead is not filled in (40 lowercase hex digits): '$head'" }
    & $Python -I -B -X utf8 $tool reviewed-head --body $BodyFile --head $head
    if ($LASTEXITCODE) { throw "release_ci.py reviewed-head failed ($LASTEXITCODE)" }
}
