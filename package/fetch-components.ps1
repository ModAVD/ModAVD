# Copyright 2026 qiyinxi
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# This program is free software: you can redistribute it and/or modify it
# under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or (at your
# option) any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
# details. You should have received a copy of it along with this program; if
# not, see <https://www.gnu.org/licenses/>.

<#
魔改 AVD：补全 Google 组件。

Release 包里只有我们自己有权分发的东西（自编模拟器、µLauncher、脚本）。
另外两样属于 Google、受《Android SDK 许可协议》约束、不能再分发，由本脚本从官方渠道下载：
  - 平台工具（adb）        platform-tools 37.0.1，约 8 MB
  - 系统镜像               Android 14（API 34）Google APIs x86_64 r14，约 1.5 GB，解压后约 4.2 GB

测试者双击根目录的「补全组件.bat」即可。流程：
  1. 从镜像取《Android SDK 许可协议》全文并显示，输入 y 同意才继续，否则什么也不下；
  2. 依次试 googledownloads.cn → 腾讯云镜像 → dl.google.com，前一个不通或校验不对就换下一个；
  3. 先下成 downloads\*.part，支持断点续传（中断后再运行会接着下），校验大小和 SHA-1 后才改名；
  4. 解压到 sdk\.staging\ 暂存，整包解完再挪到 sdk\ 下的目标位置，缺 source.properties 就补写；
  5. 已经齐全的组件跳过，重复运行不出错。

约束（改脚本时也要守住）：
- 必须能在 Windows PowerShell 5.1 上跑：不用 ??、三元、&&；文件存成 UTF-8 带 BOM。
- 只写这个文件夹：downloads\、logs\fetch-components.log、sdk\ 下两个组件目录和 sdk\.staging\。
  不碰 adb server（5037 或别的端口），不写注册表，不需要管理员。
- 必须向服务器要原始字节（Accept-Encoding: identity）：dl.google.com 在客户端声明支持 gzip 时
  会把 zip 再 gzip 一遍传，Range 续传就落在压缩流上，接起来的文件解不开。
- 不用 Invoke-WebRequest：PS 5.1 的进度条会把下载拖慢几十倍。

参数：
  -Root           模拟器根目录，默认本脚本所在目录的上一级
  -AcceptLicense  不询问，直接视为同意《Android SDK 许可协议》。仅供自动化测试使用；
                  许可协议全文照样取回并打印，取不到照样退出
  -Mirror         只用指定的一个镜像：googledownloads_cn / tencent / google
  -KeepDownloads  装好后保留 downloads\ 里的 zip（默认装好即删，省 1.5 GB）
  -ConnectTimeoutSec  等服务器回应（连接、返回响应头）的上限，默认 20 秒，超时换下一个镜像
  -StallTimeoutSec    下载或取清单时，连续这么久一个字节都没收到就算卡住，默认 30 秒，超时换下一个镜像
  -ManifestTimeoutSec 取许可协议清单（repository2-3.xml，约 420 KB）整体的上限，默认 40 秒
  -MirrorUrl      自建或测试用：用这些地址（以 / 结尾的目录地址，可写多个）代替内置的三个镜像，按给定顺序试
  -TestFreeSpaceMB  仅测试用：假装磁盘只剩这么多 MB，用来演示「空间不够」的提示

退出码：0 完成；1 下载、校验或解压失败；2 没有同意许可协议；3 文件夹不对（路径含中文、不是模拟器文件夹、空间不够）。
#>
[CmdletBinding()]
param(
    [string]$Root,
    [switch]$AcceptLicense,
    [ValidateSet('googledownloads_cn', 'tencent', 'google')]
    [string]$Mirror,
    [switch]$KeepDownloads,
    [ValidateRange(1, 600)]
    [int]$ConnectTimeoutSec = 20,
    [ValidateRange(1, 600)]
    [int]$StallTimeoutSec = 30,
    [ValidateRange(1, 1800)]
    [int]$ManifestTimeoutSec = 40,
    [string[]]$MirrorUrl,
    [long]$TestFreeSpaceMB = -1
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
# 数字格式与系统区域无关：PowerShell 7 的 -f 跟随区域设置，en-SE 等会把小数点写成逗号、千分位写成空格
[Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::InvariantCulture

if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }
$Root = [IO.Path]::GetFullPath($Root)

# ---------------------------------------------------------------- 固定参数
# 与 AUTO-MAS app/utils/emulator2/avd/constants.py 的 REQUIRED_COMPONENTS 一致；改版本时两边一起改并重新实测。

$Mirrors = @(
    @{ Id = 'googledownloads_cn'; Name = 'Google 中国下载（googledownloads.cn）'; Root = 'https://googledownloads.cn/android/repository/' },
    @{ Id = 'tencent'; Name = '腾讯云镜像（mirrors.cloud.tencent.com）'; Root = 'https://mirrors.cloud.tencent.com/AndroidSDK/' },
    @{ Id = 'google'; Name = 'Google 官方（dl.google.com）'; Root = 'https://dl.google.com/android/repository/' }
)
if ($Mirror) { $Mirrors = @($Mirrors | Where-Object { $_.Id -eq $Mirror }) }
if ($MirrorUrl) {
    $custom = @()
    # 用 -File 调起时数组参数不会按逗号拆开，这里自己拆
    foreach ($u in @($MirrorUrl | ForEach-Object { $_ -split ',' })) {
        $u = $u.Trim()
        if (-not $u) { continue }
        if (-not $u.EndsWith('/')) { $u += '/' }
        $custom += @{ Id = 'custom'; Name = ('自定义镜像（' + $u + '）'); Root = $u }
    }
    $Mirrors = $custom
}

$LicenseManifest = 'repository2-3.xml'
$LicenseId = 'android-sdk-license'

$Components = @(
    @{
        Id = 'platform-tools'; Name = '平台工具（adb）'; Version = '37.0.1'
        Url = 'platform-tools_r37.0.1-win.zip'; Size = [long]8044989; Sha1 = 'e03e78b1d80b396f1c3358e31251cb31740e1110'
        Target = 'platform-tools'; ArchiveRoot = 'platform-tools'
        KeyFiles = @('adb.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll'); ExtractedBytes = [long]20MB
        # Pinned files from the SHA-1-verified Google 37.0.1 archive.
        KeyHashes = @{
            'adb.exe' = 'b4a6b455702684652cccf7b46258b29e653538904359a58fd4931cf3ef286b3f'
            'AdbWinApi.dll' = 'c1d653030b4bde65d3e07e4d0b0979e17be56df1436cdd15528630f27808050d'
            'AdbWinUsbApi.dll' = '0710e894d9b40f71a670c13c694079d564c92c1279da382cfe4850983aaebe1b'
        }
        Props = "Pkg.UserSrc=false`nPkg.Revision=37.0.1`n"
    },
    @{
        Id = 'system-image'; Name = '系统镜像（Android 14 Google APIs x86_64）'; Version = '14'
        Url = 'sys-img/google_apis/x86_64-34_r14.zip'; Size = [long]1563721130; Sha1 = 'e0f6c9a0691aa27bd597d0deb1bcfdc943ac8ca7'
        Target = 'system-images\android-34\google_apis\x86_64'; ArchiveRoot = 'x86_64'
        KeyFiles = @('system.img', 'vendor.img', 'kernel-ranchu', 'ramdisk.img'); ExtractedBytes = [long]4500MB
        Props = "Pkg.Desc=System Image x86_64 with Google APIs.`nPkg.Revision=14`nPkg.Dependencies=emulator#34.2.16`nAndroidVersion.ApiLevel=34`nAndroidVersion.ExtensionLevel=7`nAndroidVersion.IsBaseSdk=true`nSystemImage.Abi=x86_64`nSystemImage.TagId=google_apis`nSystemImage.TagDisplay=Google APIs`nSystemImage.GpuSupport=true`nAddon.VendorId=google`nAddon.VendorDisplay=Google Inc.`n"
    }
)

$SdkDir = Join-Path $Root 'sdk'
$DownloadDir = Join-Path $Root 'downloads'
$StagingRoot = Join-Path $SdkDir '.staging'
$LogFile = Join-Path (Join-Path $Root 'logs') 'fetch-components.log'
$Interactive = -not [Console]::IsOutputRedirected
$UserAgent = 'ModAVD-fetch-components/1.0'

# 本次出错时报告用：哪个组件、哪个镜像、哪一步
$script:Where = ''

# ---------------------------------------------------------------- 工具函数

function Write-Log([string]$Text) {
    try {
        $dir = Split-Path -Parent $LogFile
        if (-not (Test-Path -LiteralPath $dir)) { [void](New-Item -ItemType Directory -Path $dir) }
        [IO.File]::AppendAllText($LogFile, ('{0:yyyy-MM-dd HH:mm:ss} {1}' -f (Get-Date), $Text) + "`r`n", (New-Object Text.UTF8Encoding($false)))
    } catch { }
}

function Say([string]$Text, [string]$Color) {
    if ($Color) { Write-Host $Text -ForegroundColor $Color } else { Write-Host $Text }
    Write-Log $Text
}

function Format-Size([double]$Bytes) {
    if ($Bytes -ge 1GB) { return ('{0:0.00} GB' -f ($Bytes / 1GB)) }
    if ($Bytes -ge 1MB) { return ('{0:0.0} MB' -f ($Bytes / 1MB)) }
    if ($Bytes -ge 1KB) { return ('{0:0} KB' -f ($Bytes / 1KB)) }
    return ('{0:0} B' -f $Bytes)
}

function Format-Duration([double]$Seconds) {
    if ($Seconds -lt 0 -or [double]::IsInfinity($Seconds) -or [double]::IsNaN($Seconds)) { return '--:--' }
    $t = [TimeSpan]::FromSeconds([math]::Round($Seconds))
    if ($t.TotalHours -ge 1) { return ('{0}:{1:00}:{2:00}' -f [int][math]::Floor($t.TotalHours), $t.Minutes, $t.Seconds) }
    return ('{0:00}:{1:00}' -f $t.Minutes, $t.Seconds)
}

# 进度：交互窗口里原地刷新一行；输出被重定向时（自动化）每 10 秒打一行
$script:LastShown = [DateTime]::MinValue
$script:LineWidth = 0
function Show-Progress([string]$Text, [bool]$Final) {
    $now = [DateTime]::UtcNow
    if ($Interactive) {
        if (-not $Final -and ($now - $script:LastShown).TotalMilliseconds -lt 500) { return }
        $script:LastShown = $now
        $pad = [math]::Max(0, $script:LineWidth - $Text.Length)
        Write-Host -NoNewline ("`r" + $Text + (' ' * $pad))
        $script:LineWidth = $Text.Length
        if ($Final) { Write-Host ''; $script:LineWidth = 0 }
    } else {
        if (-not $Final -and ($now - $script:LastShown).TotalSeconds -lt 10) { return }
        $script:LastShown = $now
        Write-Host $Text
    }
}
function Reset-Progress { $script:LastShown = [DateTime]::MinValue; $script:LineWidth = 0 }

function New-Request([string]$Url) {
    $req = [Net.HttpWebRequest]::Create($Url)
    $req.Method = 'GET'
    $req.UserAgent = $UserAgent
    $req.AllowAutoRedirect = $true
    # 不让 .NET 自动解压，也明确告诉服务器只要原始字节（见文件头「约束」）
    $req.AutomaticDecompression = [Net.DecompressionMethods]::None
    $req.Headers.Add('Accept-Encoding', 'identity')
    # 超时不靠 HttpWebRequest.Timeout（它管不到 DNS 和读响应体），而是下面 Wait-Task 自己计时，到点 Abort
    $req.Timeout = ($ConnectTimeoutSec + 10) * 1000
    $req.ReadWriteTimeout = ($StallTimeoutSec + 10) * 1000
    return $req
}

# 等一个异步任务最多 $Seconds 秒；到点就 Abort 请求并抛「超时：…」，任务自己出错则把真正的异常抛出来
function Wait-Task($Task, [int]$Seconds, $Request, [string]$What) {
    $finished = $false
    $err = $null
    try { $finished = $Task.Wait($Seconds * 1000) } catch { $finished = $true; $err = $_.Exception }
    if (-not $finished) {
        try { $Request.Abort() } catch { }
        throw ('超时：{0}，{1} 秒内没有进展' -f $What, $Seconds)
    }
    if ($err) {
        # PowerShell 调 .Wait() 出错时外面包了 MethodInvocationException，里面才是 AggregateException
        while ($err.InnerException -and ($err -is [Management.Automation.MethodInvocationException] -or $err -is [AggregateException])) { $err = $err.InnerException }
        throw $err
    }
}

function Get-Response($Request, [string]$What) {
    $task = $Request.GetResponseAsync()
    try {
        Wait-Task $task $ConnectTimeoutSec $Request ($What + '（等服务器回应）')
    } catch [Net.WebException] {
        $r = $_.Exception.Response
        if ($r) {
            $code = [int]$r.StatusCode
            $r.Close()
            throw ('服务器返回 HTTP {0}' -f $code)
        }
        throw ('连接失败：' + $_.Exception.Message)
    }
    return $task.Result
}

function Assert-Identity($Response) {
    $ce = $Response.Headers['Content-Encoding']
    if ($ce -and $ce -ne 'identity') { throw ('服务器返回了压缩过的内容（Content-Encoding: ' + $ce + '），没法校验和续传') }
}

function Get-Bytes([string]$Url, [string]$What, [int]$TotalSec) {
    $req = New-Request $Url
    $resp = Get-Response $req $What
    try {
        Assert-Identity $resp
        $ms = New-Object IO.MemoryStream
        $s = $resp.GetResponseStream()
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $buf = New-Object byte[] (64KB)
        try {
            while ($true) {
                $left = $TotalSec - $sw.Elapsed.TotalSeconds
                if ($left -le 0) {
                    try { $req.Abort() } catch { }
                    throw ('超时：{0}，整个过程超过 {1} 秒还没取完' -f $What, $TotalSec)
                }
                $t = $s.ReadAsync($buf, 0, $buf.Length)
                $wait = [int][math]::Min($StallTimeoutSec, [math]::Ceiling($left))
                Wait-Task $t $wait $req ($What + '（读数据）')
                $n = $t.Result
                if ($n -le 0) { break }
                $ms.Write($buf, 0, $n)
            }
        } finally { $s.Dispose() }
        return , $ms.ToArray()
    } finally { $resp.Close() }
}

# 用 .NET 的 SHA1 流式计算，不用 Get-FileHash：经 cmd 调起的 Windows PowerShell 5.1 有时找不到它
function Get-Sha1([string]$Path) {
    $sha = [Security.Cryptography.SHA1]::Create()
    try {
        $fs = New-Object IO.FileStream($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read, 1MB)
        try {
            $hash = $sha.ComputeHash($fs)
        } finally { $fs.Dispose() }
    } finally { $sha.Dispose() }
    return ([BitConverter]::ToString($hash).Replace('-', '')).ToLowerInvariant()
}

function Get-Sha256([string]$Path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $fs = New-Object IO.FileStream($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read, 1MB)
        try {
            $hash = $sha.ComputeHash($fs)
        } finally { $fs.Dispose() }
    } finally { $sha.Dispose() }
    return ([BitConverter]::ToString($hash).Replace('-', '')).ToLowerInvariant()
}

function Remove-File([string]$Path) {
    # 删掉丢弃的 .part（重新下载用）
    if (Test-Path -LiteralPath $Path) { [IO.File]::Delete($Path) }
}

function Remove-Staging([string]$Dir) {
    # 只删 sdk\.staging 下的暂存目录
    if (-not $Dir) { return }
    $full = [IO.Path]::GetFullPath($Dir)
    if (-not $full.StartsWith($StagingRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { return }
    if (Test-Path -LiteralPath $full) { [IO.Directory]::Delete($full, $true) }
    if ((Test-Path -LiteralPath $StagingRoot) -and -not (Get-ChildItem -LiteralPath $StagingRoot -Force | Select-Object -First 1)) {
        [IO.Directory]::Delete($StagingRoot)
    }
}

function Clear-StagingRoot {
    # 上次被强行结束时可能留下的暂存目录，整个 sdk\.staging 都是本脚本自己的，直接清掉
    if (-not (Test-Path -LiteralPath $StagingRoot)) { return }
    foreach ($d in @(Get-ChildItem -LiteralPath $StagingRoot -Force)) {
        if ($d.PSIsContainer) { Remove-Staging $d.FullName } else { [IO.File]::Delete($d.FullName) }
    }
    if (Test-Path -LiteralPath $StagingRoot) { [IO.Directory]::Delete($StagingRoot, $true) }
    Write-Log '  清理了上次遗留的 sdk\.staging'
}

function Test-ComponentFiles($C, [string]$Dir) {
    try {
        foreach ($k in $C.KeyFiles) {
            $p = Join-Path $Dir $k
            if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { return $false }
            if ((Get-Item -LiteralPath $p).Length -le 0) { return $false }
        }
        if ($C.KeyHashes) {
            foreach ($entry in $C.KeyHashes.GetEnumerator()) {
                $hash = (Get-Sha256 (Join-Path $Dir $entry.Key))
                if ($hash -ine $entry.Value) { return $false }
            }
            $props = Join-Path $Dir 'source.properties'
            if (-not (Test-Path -LiteralPath $props -PathType Leaf)) { return $false }
            $revision = @([IO.File]::ReadAllLines($props) | Where-Object { $_ -match '^\s*Pkg\.Revision\s*=' })
            if ($revision.Count -ne 1 -or ($revision[0] -replace '^\s*Pkg\.Revision\s*=\s*', '').Trim() -ne $C.Version) { return $false }
        }
        return $true
    } catch {
        return $false
    }
}

function Test-Installed($C) {
    return (Test-ComponentFiles $C (Join-Path $SdkDir $C.Target))
}

function Get-Revision($C) {
    $p = Join-Path (Join-Path $SdkDir $C.Target) 'source.properties'
    if (-not (Test-Path -LiteralPath $p)) { return '' }
    foreach ($line in [IO.File]::ReadAllLines($p)) {
        if ($line -match '^\s*Pkg\.Revision\s*=\s*(.+?)\s*$') { return $Matches[1] }
    }
    return ''
}

# ---------------------------------------------------------------- 下载

function Invoke-Download([string]$Url, [string]$Part, [long]$Expected, [string]$Label) {
    $existing = [long]0
    if (Test-Path -LiteralPath $Part) { $existing = (Get-Item -LiteralPath $Part).Length }
    if ($existing -gt $Expected) { Remove-File $Part; $existing = 0 }
    if ($existing -eq $Expected) { return }

    $req = New-Request $Url
    if ($existing -gt 0) { $req.AddRange([long]$existing) }
    $resp = Get-Response $req ('下载 ' + $Label)
    $fs = $null
    $stream = $null
    try {
        Assert-Identity $resp
        $status = [int]$resp.StatusCode
        $mode = [IO.FileMode]::Append
        if ($existing -gt 0) {
            $cr = $resp.Headers['Content-Range']
            if ($status -eq 206 -and $cr -and $cr.StartsWith('bytes ' + $existing + '-')) {
                if (-not $cr.EndsWith('/' + $Expected)) { throw ('文件大小和预期不符（Content-Range: ' + $cr + '，应为 ' + $Expected + ' 字节）') }
                Say ('  从 {0} 处接着下' -f (Format-Size $existing))
            } elseif ($status -eq 200) {
                Say '  这个镜像不支持断点续传，从头下载' 'Yellow'
                $existing = 0
                $mode = [IO.FileMode]::Create
            } else {
                throw ('断点续传的回应不对（HTTP {0}，Content-Range: {1}）' -f $status, $cr)
            }
        } else {
            if ($status -ne 200) { throw ('服务器返回 HTTP {0}' -f $status) }
            $mode = [IO.FileMode]::Create
        }
        if ($status -eq 200 -and $resp.ContentLength -ge 0 -and $resp.ContentLength -ne $Expected) {
            throw ('文件大小和预期不符（服务器给 {0} 字节，应为 {1} 字节）' -f $resp.ContentLength, $Expected)
        }

        $fs = New-Object IO.FileStream($Part, $mode, [IO.FileAccess]::Write, [IO.FileShare]::Read, 1MB)
        $stream = $resp.GetResponseStream()
        $buf = New-Object byte[] (1MB)
        $done = $existing
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $sampleT = 0.0
        $sampleB = $done
        $speed = 0.0
        Reset-Progress
        while ($true) {
            $rt = $stream.ReadAsync($buf, 0, $buf.Length)
            Wait-Task $rt $StallTimeoutSec $req ('下载 ' + $Label + '（收数据，已下 ' + (Format-Size $done) + '）')
            $n = $rt.Result
            if ($n -le 0) { break }
            $fs.Write($buf, 0, $n)
            $done += $n
            $t = $sw.Elapsed.TotalSeconds
            if ($t - $sampleT -ge 1.0) {
                $inst = ($done - $sampleB) / ($t - $sampleT)
                if ($speed -le 0) { $speed = $inst } else { $speed = 0.7 * $speed + 0.3 * $inst }
                $sampleT = $t
                $sampleB = $done
            }
            $eta = -1
            if ($speed -gt 0) { $eta = ($Expected - $done) / $speed }
            Show-Progress ('  {0}  {1,5:0.0}%  {2} / {3}  {4}/s  剩余 {5}' -f $Label, (100.0 * $done / $Expected), (Format-Size $done), (Format-Size $Expected), (Format-Size $speed), (Format-Duration $eta)) $false
        }
        $fs.Flush()
        $avg = 0
        if ($sw.Elapsed.TotalSeconds -gt 0) { $avg = ($done - $existing) / $sw.Elapsed.TotalSeconds }
        Show-Progress ('  {0}  {1,5:0.0}%  {2} / {3}  平均 {4}/s' -f $Label, (100.0 * $done / $Expected), (Format-Size $done), (Format-Size $Expected), (Format-Size $avg)) $true
        Write-Log ('  下载结束：{0} 字节，用时 {1:0.0} 秒' -f ($done - $existing), $sw.Elapsed.TotalSeconds)
    } finally {
        if ($stream) { $stream.Dispose() }
        if ($fs) { $fs.Dispose() }
        $resp.Close()
    }
    $size = (Get-Item -LiteralPath $Part).Length
    if ($size -ne $Expected) { throw ('下载不完整：应为 {0} 字节，实际 {1} 字节' -f $Expected, $size) }
}

# 下好一个组件的 zip（含镜像回落与重试），返回 zip 路径
function Get-ComponentZip($C) {
    if (-not (Test-Path -LiteralPath $DownloadDir)) { [void](New-Item -ItemType Directory -Path $DownloadDir) }
    $zip = Join-Path $DownloadDir ([IO.Path]::GetFileName($C.Url))
    $part = $zip + '.part'

    if (Test-Path -LiteralPath $zip) {
        $script:Where = '校验上次下好的 ' + [IO.Path]::GetFileName($zip)
        Say ('  发现上次下好的 {0}，校验中…' -f [IO.Path]::GetFileName($zip))
        if ((Get-Item -LiteralPath $zip).Length -eq $C.Size -and (Get-Sha1 $zip) -eq $C.Sha1) {
            Say '  校验通过，直接用' 'Green'
            return $zip
        }
        Say '  校验不通过，重新下载' 'Yellow'
        Remove-File $zip
    }

    $errors = @()
    foreach ($m in $Mirrors) {
        $url = $m.Root + $C.Url
        Say ('  镜像：{0}' -f $m.Name)
        Write-Log ('  URL ' + $url)
        $ok = $false
        for ($attempt = 1; $attempt -le 3; $attempt++) {
            try {
                $script:Where = '从「' + $m.Name + '」下载 ' + $C.Url
                Invoke-Download $url $part $C.Size $C.Name
                $ok = $true
                break
            } catch {
                $msg = $_.Exception.Message
                if ($Interactive) { Write-Host '' }
                Say ('  第 {0} 次下载出错（{1}）：{2}' -f $attempt, $m.Name, $msg) 'Yellow'
                $errors += ('{0} 下载：{1}' -f $m.Name, $msg)
                # 大小不符、HTTP 4xx 这类换镜像也重试不好，直接换下一个
                # 超时也直接换下一个镜像（已下的部分保留，下一个镜像接着下）
                if ($msg -match '大小和预期不符|HTTP 4\d\d|压缩过的内容|^超时') { break }
                if ($attempt -lt 3) { Say '  3 秒后重试（已下载的部分保留，接着下）'; Start-Sleep -Seconds 3 }
            }
        }
        if (-not $ok) { Say ('  {0} 下载失败，换下一个镜像' -f $m.Name) 'Yellow'; continue }

        $script:Where = '校验从「' + $m.Name + '」下载的 ' + $C.Url
        Say '  校验 SHA-1…'
        $sha = Get-Sha1 $part
        if ($sha -ne $C.Sha1) {
            Say ('  SHA-1 不对：{0}，应为 {1}；丢弃，换下一个镜像' -f $sha, $C.Sha1) 'Yellow'
            $errors += ('{0} 校验：SHA-1 为 {1}，应为 {2}' -f $m.Name, $sha, $C.Sha1)
            Remove-File $part
            continue
        }
        Say '  校验通过' 'Green'
        Move-Item -LiteralPath $part -Destination $zip -Force
        return $zip
    }
    $script:Where = '下载 ' + $C.Url
    throw ('所有镜像都没下成：' + ($errors -join '；'))
}

# ---------------------------------------------------------------- 解压与安装

# 自己读 zip。Windows PowerShell 5.1（.NET Framework）的 ZipArchive 打不开 Zip64 的大条目
# （系统镜像里 system.img 有 4 GB，Open() 会报 "A local file header is corrupt"），
# 所以读中央目录，按偏移定位后直接用 DeflateStream 解；PowerShell 7 上走同一份代码。
$script:U32Max = [long]4294967295

function Read-Exact([IO.Stream]$Stream, [byte[]]$Buffer, [int]$Count) {
    $got = 0
    while ($got -lt $Count) {
        $n = $Stream.Read($Buffer, $got, $Count - $got)
        if ($n -le 0) { throw '压缩包被截断（文件比目录里记的短）' }
        $got += $n
    }
}

function Read-ZipEntries([IO.FileStream]$Fs) {
    $len = $Fs.Length
    $tailLen = [int][math]::Min($len, 65557)
    $tail = New-Object byte[] $tailLen
    [void]$Fs.Seek($len - $tailLen, [IO.SeekOrigin]::Begin)
    Read-Exact $Fs $tail $tailLen
    $e = -1
    for ($i = $tailLen - 22; $i -ge 0; $i--) {
        if ($tail[$i] -eq 0x50 -and $tail[$i + 1] -eq 0x4B -and $tail[$i + 2] -eq 5 -and $tail[$i + 3] -eq 6) { $e = $i; break }
    }
    if ($e -lt 0) { throw '不是有效的 zip（找不到目录结尾）' }
    $count = [long][BitConverter]::ToUInt16($tail, $e + 10)
    $cdSize = [long][BitConverter]::ToUInt32($tail, $e + 12)
    $cdOff = [long][BitConverter]::ToUInt32($tail, $e + 16)
    if ($count -eq 65535 -or $cdSize -eq $script:U32Max -or $cdOff -eq $script:U32Max) {
        $loc = $e - 20
        if ($loc -lt 0 -or [BitConverter]::ToUInt32($tail, $loc) -ne 0x07064B50) { throw 'zip 的 Zip64 目录定位记录损坏' }
        $z64 = [BitConverter]::ToInt64($tail, $loc + 8)
        $hdr = New-Object byte[] 56
        [void]$Fs.Seek($z64, [IO.SeekOrigin]::Begin)
        Read-Exact $Fs $hdr 56
        if ([BitConverter]::ToUInt32($hdr, 0) -ne 0x06064B50) { throw 'zip 的 Zip64 目录结尾记录损坏' }
        $count = [BitConverter]::ToInt64($hdr, 32)
        $cdSize = [BitConverter]::ToInt64($hdr, 40)
        $cdOff = [BitConverter]::ToInt64($hdr, 48)
    }
    if ($cdSize -gt 64MB) { throw 'zip 的目录大得不正常' }
    $cd = New-Object byte[] ([int]$cdSize)
    [void]$Fs.Seek($cdOff, [IO.SeekOrigin]::Begin)
    Read-Exact $Fs $cd ([int]$cdSize)

    $list = New-Object Collections.ArrayList
    $p = 0
    for ($k = 0; $k -lt $count; $k++) {
        if ($p + 46 -gt $cd.Length -or [BitConverter]::ToUInt32($cd, $p) -ne 0x02014B50) { throw 'zip 的目录项损坏' }
        $flags = [BitConverter]::ToUInt16($cd, $p + 8)
        $method = [BitConverter]::ToUInt16($cd, $p + 10)
        $dosTime = [int][BitConverter]::ToUInt16($cd, $p + 12)
        $dosDate = [int][BitConverter]::ToUInt16($cd, $p + 14)
        $csize = [long][BitConverter]::ToUInt32($cd, $p + 20)
        $usize = [long][BitConverter]::ToUInt32($cd, $p + 24)
        $nl = [int][BitConverter]::ToUInt16($cd, $p + 28)
        $el = [int][BitConverter]::ToUInt16($cd, $p + 30)
        $cl = [int][BitConverter]::ToUInt16($cd, $p + 32)
        $off = [long][BitConverter]::ToUInt32($cd, $p + 42)
        $name = [Text.Encoding]::UTF8.GetString($cd, $p + 46, $nl)
        $ex = $p + 46 + $nl
        $q = 0
        while ($q + 4 -le $el) {
            $id = [BitConverter]::ToUInt16($cd, $ex + $q)
            $sz = [int][BitConverter]::ToUInt16($cd, $ex + $q + 2)
            if ($id -eq 1) {
                $r = $ex + $q + 4
                if ($usize -eq $script:U32Max) { $usize = [BitConverter]::ToInt64($cd, $r); $r += 8 }
                if ($csize -eq $script:U32Max) { $csize = [BitConverter]::ToInt64($cd, $r); $r += 8 }
                if ($off -eq $script:U32Max) { $off = [BitConverter]::ToInt64($cd, $r); $r += 8 }
            }
            $q += 4 + $sz
        }
        $when = [DateTime]::Now
        try {
            $when = New-Object DateTime ((($dosDate -shr 9) + 1980), (($dosDate -shr 5) -band 15), ($dosDate -band 31), ($dosTime -shr 11), (($dosTime -shr 5) -band 63), (($dosTime -band 31) * 2))
        } catch { }
        [void]$list.Add([pscustomobject]@{ Name = $name; Flags = $flags; Method = $method; CSize = $csize; USize = $usize; Offset = $off; When = $when })
        $p += 46 + $nl + $el + $cl
    }
    return $list.ToArray()
}

function Install-Component($C, [string]$Zip) {
    Add-Type -AssemblyName System.IO.Compression
    $staging = Join-Path $StagingRoot $C.Id
    $script:Where = '解压 ' + [IO.Path]::GetFileName($Zip) + ' 到 ' + $staging
    Remove-Staging $staging
    [void](New-Item -ItemType Directory -Path $staging -Force)
    $stagingFull = [IO.Path]::GetFullPath($staging) + '\'
    $prefix = $C.ArchiveRoot.TrimEnd('/') + '/'

    try {
        $zfs = New-Object IO.FileStream($Zip, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read, 1MB)
        try {
            $entries = @(Read-ZipEntries $zfs)
            $total = [long]0
            foreach ($e in $entries) { $total += $e.USize }
            if ($total -le 0) { $total = 1 }
            $done = [long]0
            $buf = New-Object byte[] (1MB)
            $lh = New-Object byte[] 30
            Reset-Progress
            foreach ($e in $entries) {
                $name = $e.Name.Replace('\', '/')
                if (-not $name.StartsWith($prefix)) { throw ('压缩包结构不对：有不在 ' + $prefix + ' 下的条目 ' + $e.Name) }
                $dest = [IO.Path]::GetFullPath((Join-Path $staging ($name.Replace('/', '\'))))
                if (-not $dest.StartsWith($stagingFull, [StringComparison]::OrdinalIgnoreCase)) { throw ('压缩包里有非法路径：' + $e.Name) }
                if ($name.EndsWith('/')) {
                    if (-not (Test-Path -LiteralPath $dest)) { [void](New-Item -ItemType Directory -Path $dest) }
                    continue
                }
                if (($e.Flags -band 1) -ne 0) { throw ('压缩包里的 ' + $e.Name + ' 是加密的') }
                $dd = Split-Path -Parent $dest
                if (-not (Test-Path -LiteralPath $dd)) { [void](New-Item -ItemType Directory -Path $dd) }
                # 定位到数据起点：本地文件头 30 字节 + 文件名 + 额外字段
                [void]$zfs.Seek($e.Offset, [IO.SeekOrigin]::Begin)
                Read-Exact $zfs $lh 30
                if ([BitConverter]::ToUInt32($lh, 0) -ne 0x04034B50) { throw ('压缩包里 ' + $e.Name + ' 的文件头损坏') }
                $dataOff = $e.Offset + 30 + [BitConverter]::ToUInt16($lh, 26) + [BitConverter]::ToUInt16($lh, 28)
                [void]$zfs.Seek($dataOff, [IO.SeekOrigin]::Begin)
                $in = $null
                if ($e.Method -eq 0) { $in = $zfs }
                elseif ($e.Method -eq 8) { $in = New-Object IO.Compression.DeflateStream($zfs, [IO.Compression.CompressionMode]::Decompress, $true) }
                else { throw ('压缩包里 ' + $e.Name + ' 用了不支持的压缩方式 ' + $e.Method) }
                # 手动流式解压（system.img 一个文件就 4 GB，要边解边报进度）
                try {
                    $out = New-Object IO.FileStream($dest, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None, 1MB)
                    try {
                        $remaining = $e.USize
                        while ($remaining -gt 0) {
                            $want = [int][math]::Min([long]$buf.Length, $remaining)
                            $n = $in.Read($buf, 0, $want)
                            if ($n -le 0) { throw ('压缩数据提前结束：' + $e.Name) }
                            $out.Write($buf, 0, $n)
                            $remaining -= $n
                            $done += $n
                            Show-Progress ('  解压 {0}  {1,5:0.0}%  {2} / {3}' -f $C.Name, (100.0 * $done / $total), (Format-Size $done), (Format-Size $total)) $false
                        }
                    } finally { $out.Dispose() }
                } finally { if ($e.Method -ne 0) { $in.Dispose() } }
                if ((Get-Item -LiteralPath $dest).Length -ne $e.USize) { throw ('解压出的 ' + $e.Name + ' 大小不对') }
                try { [IO.File]::SetLastWriteTime($dest, $e.When) } catch { }
            }
            Show-Progress ('  解压 {0}  100.0%  {1}' -f $C.Name, (Format-Size $done)) $true
        } finally { $zfs.Dispose() }

        $extracted = Join-Path $staging $C.ArchiveRoot
        if (-not (Test-Path -LiteralPath $extracted -PathType Container)) { throw ('压缩包结构不对：没有 ' + $prefix + ' 目录') }
        foreach ($k in $C.KeyFiles) {
            if (-not (Test-Path -LiteralPath (Join-Path $extracted $k) -PathType Leaf)) { throw ('压缩包里缺少 ' + $k) }
        }
        $props = Join-Path $extracted 'source.properties'
        if (-not (Test-Path -LiteralPath $props)) {
            [IO.File]::WriteAllText($props, $C.Props, (New-Object Text.UTF8Encoding($false)))
            Say '  压缩包里没有 source.properties，已补写'
        }

        if (-not (Test-ComponentFiles $C $extracted)) {
            throw ('解压后的关键文件或版本校验失败：' + $C.Name)
        }

        $script:Where = '把解压好的 ' + $C.Name + ' 挪到 sdk\' + $C.Target
        $target = Join-Path $SdkDir $C.Target
        if (Test-Path -LiteralPath $target) {
            # 不完整的旧目录挪开不删，留给用户自己处理
            $aside = $target + '.incomplete-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
            Move-Item -LiteralPath $target -Destination $aside
            Say ('  原有的 sdk\{0} 不完整，已改名为 {1}，确认没用后可以自己删掉' -f $C.Target, [IO.Path]::GetFileName($aside)) 'Yellow'
        }
        $parent = Split-Path -Parent $target
        if (-not (Test-Path -LiteralPath $parent)) { [void](New-Item -ItemType Directory -Path $parent -Force) }
        [IO.Directory]::Move($extracted, $target)
    } finally {
        Remove-Staging $staging
    }
    if (-not (Test-Installed $C)) { throw ('装完后 sdk\' + $C.Target + ' 仍缺关键文件') }
}

# ---------------------------------------------------------------- 主流程

function Main {
    Write-Host ''
    Say '魔改 AVD：补全 Google 组件（adb、Android 14 系统镜像）'
    Say ('文件夹：' + $Root)
    Write-Log ('PowerShell ' + $PSVersionTable.PSVersion + '，参数：AcceptLicense=' + [bool]$AcceptLicense + ' Mirror=' + $Mirror + ' KeepDownloads=' + [bool]$KeepDownloads)
    Write-Host ''

    if ($Root -match '[^\x00-\x7F]') {
        Say '这个文件夹的路径里有中文或其他非英文字符，模拟器在这种路径下起不来。' 'Red'
        Say '请把整个文件夹挪到只含英文、数字的路径下（例如 D:\ModAVD），再重新运行。' 'Red'
        return 3
    }
    if (-not (Test-Path -LiteralPath (Join-Path $SdkDir 'emulator\emulator.exe'))) {
        Say ('这里没有 sdk\emulator\emulator.exe，不像是魔改 AVD 的文件夹：' + $Root) 'Red'
        Say '请把 Release 压缩包完整解压后，在解压出来的文件夹里双击「补全组件.bat」。' 'Red'
        return 3
    }

    Clear-StagingRoot

    # 已经齐全的跳过
    $todo = @()
    foreach ($c in $Components) {
        if (Test-Installed $c) {
            $rev = Get-Revision $c
            Say ('已有：{0}（版本 {1}），跳过' -f $c.Name, $(if ($rev) { $rev } else { '未知' })) 'Green'
            if ($rev -and $rev -ne $c.Version) { Say ('  注意：这个版本和本包测试过的 {0} 不同' -f $c.Version) 'Yellow' }
        } else {
            $todo += $c
        }
    }
    if ($todo.Count -eq 0) {
        Write-Host ''
        Say '组件都已齐全，不需要下载。完成，可以在 AUTO-MAS 里添加这个文件夹了。' 'Green'
        return 0
    }

    # 空间：下载（扣掉已下的部分）+ 解压，再留 500 MB 余量
    $need = [long]500MB
    foreach ($c in $todo) {
        $zip = Join-Path $DownloadDir ([IO.Path]::GetFileName($c.Url))
        $have = [long]0
        if (Test-Path -LiteralPath $zip) { $have = (Get-Item -LiteralPath $zip).Length }
        elseif (Test-Path -LiteralPath ($zip + '.part')) { $have = (Get-Item -LiteralPath ($zip + '.part')).Length }
        $need += [math]::Max([long]0, $c.Size - $have) + $c.ExtractedBytes
    }
    $free = (New-Object IO.DriveInfo([IO.Path]::GetPathRoot($Root))).AvailableFreeSpace
    if ($TestFreeSpaceMB -ge 0) { $free = $TestFreeSpaceMB * 1MB; Write-Log ('  （测试：假装只剩 {0} MB）' -f $TestFreeSpaceMB) }
    if ($free -lt $need) {
        Say ('磁盘空间不够：还需要约 {0}，{1} 只剩 {2}。游戏数据以后也放在这个文件夹里，请换一个空间更大的盘。' -f (Format-Size $need), [IO.Path]::GetPathRoot($Root), (Format-Size $free)) 'Red'
        return 3
    }

    try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch { }

    # 1. 许可协议
    $script:Where = '获取许可协议'
    $licenseText = $null
    $licenseFrom = ''
    $licenseErrors = @()
    $licenseFailed = @()
    foreach ($m in $Mirrors) {
        try {
            $script:Where = '从「' + $m.Name + '」获取许可协议清单 ' + $LicenseManifest
            $bytes = Get-Bytes ($m.Root + $LicenseManifest) ('取许可协议清单 ' + $LicenseManifest) $ManifestTimeoutSec
            $xml = New-Object Xml.XmlDocument
            $xml.XmlResolver = $null
            $xml.LoadXml([Text.Encoding]::UTF8.GetString($bytes).TrimStart([char]0xFEFF))
            $node = $xml.SelectSingleNode("//*[local-name()='license' and @id='" + $LicenseId + "']")
            if (-not $node -or -not $node.InnerText.Trim()) { throw ('清单里没有许可协议 ' + $LicenseId) }
            $licenseText = $node.InnerText.Trim()
            $licenseFrom = $m.Name
            break
        } catch {
            Say ('从 {0} 取许可协议失败：{1}' -f $m.Name, $_.Exception.Message) 'Yellow'
            $licenseErrors += ('{0}：{1}' -f $m.Name, $_.Exception.Message)
            $licenseFailed += $m.Name
            if ($m -ne $Mirrors[$Mirrors.Count - 1]) { Say '  换下一个镜像' }
        }
    }
    if (-not $licenseText) {
        Say '取不到《Android SDK 许可协议》全文，什么也没有下载。请检查网络后重试。' 'Red'
        Say ('失败：获取许可协议清单 ' + $LicenseManifest + '，试过的镜像都不行') 'Red'
        Say ('各镜像的情况：' + ($licenseErrors -join '；')) 'Red'
        Say ('详细记录：' + $LogFile)
        return 1
    }
    if ($licenseFailed.Count -gt 0) {
        # 连清单都取不到的镜像排到最后，省得下载时每个组件再白等一次超时
        $script:Mirrors = @(@($Mirrors | Where-Object { $licenseFailed -notcontains $_.Name }) + @($Mirrors | Where-Object { $licenseFailed -contains $_.Name }))
        Say ('  下载时先用能取到清单的镜像，{0} 排到最后' -f ($licenseFailed -join '、'))
    }
    $licenseFile = Join-Path $DownloadDir 'android-sdk-license.txt'

    Write-Host ''
    Write-Host '================ Android Software Development Kit License Agreement ================' -ForegroundColor Cyan
    Write-Host $licenseText
    Write-Host '=====================================================================================' -ForegroundColor Cyan
    Write-Host ''
    Say ('以上是 Google《Android 软件开发套件许可协议》全文（取自 {0}）。' -f $licenseFrom)
    Say ('接下来要下载的 {0} 由 Google 提供、受这份协议约束，本项目不分发它们。' -f (($todo | ForEach-Object { $_.Name }) -join '、'))
    if ($AcceptLicense) {
        Say '已通过 -AcceptLicense 参数同意许可协议。'
    } else {
        Write-Host '是否同意这份许可协议？输入 y 并回车表示同意，直接回车或输入其他内容则退出：' -ForegroundColor Cyan -NoNewline
        $answer = [Console]::In.ReadLine()
        Write-Log ('用户输入：' + $answer)
        if (-not $answer -or $answer.Trim() -notmatch '^(y|yes)$') {
            Write-Host ''
            Say '没有同意许可协议，已退出，什么也没有下载。' 'Yellow'
            return 2
        }
        Say '已同意许可协议。'
    }
    # 同意之后才留许可协议副本；拒绝时 downloads\ 里不会多出任何文件
    if (-not (Test-Path -LiteralPath $DownloadDir)) { [void](New-Item -ItemType Directory -Path $DownloadDir) }
    [IO.File]::WriteAllText($licenseFile, ($licenseText -replace "`r?`n", "`r`n") + "`r`n", (New-Object Text.UTF8Encoding($true)))
    Say ('许可协议另存了一份在：' + $licenseFile)

    # 2. 逐个下载、校验、安装
    foreach ($c in $todo) {
        Write-Host ''
        Say ('—— {0}（{1}）' -f $c.Name, (Format-Size $c.Size)) 'Cyan'
        $zip = Get-ComponentZip $c
        Say '  解压安装…'
        Install-Component $c $zip
        Say ('  已装好：sdk\' + $c.Target) 'Green'
        if (-not $KeepDownloads) {
            [IO.File]::Delete($zip)
            Write-Log ('  已删除下载的 ' + $zip)
        }
    }
    if ((Test-Path -LiteralPath $DownloadDir) -and -not (Get-ChildItem -LiteralPath $DownloadDir -Force | Where-Object { $_.Name -ne 'android-sdk-license.txt' } | Select-Object -First 1)) {
        Write-Log '  downloads\ 里只剩许可协议副本'
    }

    foreach ($c in $Components) {
        if (-not (Test-Installed $c)) { throw ('检查发现 ' + $c.Name + ' 仍不完整') }
    }
    Write-Host ''
    Say '完成，可以在 AUTO-MAS 里添加这个文件夹了。' 'Green'
    return 0
}

$code = 1
try {
    $r = @(Main)
    $code = [int]$r[$r.Count - 1]
} catch {
    if ($Interactive) { Write-Host '' }
    Say ('失败：' + $script:Where) 'Red'
    Say ('原因：' + $_.Exception.Message) 'Red'
    Write-Log ($_.ScriptStackTrace)
    if ((Test-Path -LiteralPath $DownloadDir) -and (Get-ChildItem -LiteralPath $DownloadDir -Force | Where-Object { $_.Name -match '\.(part|zip)$' } | Select-Object -First 1)) {
        Say '已下载的部分保存在 downloads\ 里，再次运行会接着下。sdk\ 里没有留下装到一半的目录。'
    }
    try { Clear-StagingRoot } catch { }
    Say ('详细记录：' + $LogFile)
    $code = 1
}
Write-Log ('退出码 ' + $code)
exit $code
