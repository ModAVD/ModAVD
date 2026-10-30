# Copyright 2026 qiyinxi
# SPDX-License-Identifier: AGPL-3.0-or-later (https://www.gnu.org/licenses/agpl-3.0.html)
#
# Build the companion (x86_64) with the NDK.
#
# NOTE: each make override must be ONE pre-built string argument. Writing
# NDK_LIBS_OUT=(Join-Path ...) makes PowerShell emit two arguments and make then
# treats the path as a goal and silently builds nothing.
param(
  [string]$Ndk = $env:ANDROID_NDK_ROOT,
  [string]$OutDir
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot            # bridge
$src  = Join-Path $root 'companion'
if (-not $Ndk) { throw 'Android NDK not given: set ANDROID_NDK_ROOT to an NDK r27c, or pass -Ndk' }
$ndkBuild = Join-Path $Ndk 'ndk-build.cmd'
if (-not (Test-Path $ndkBuild)) { throw "ndk-build not found: $ndkBuild (pass -Ndk or set ANDROID_NDK_ROOT)" }
if (-not $OutDir) { $OutDir = Join-Path $env:TEMP 'modavd-nbbridge\companion' }
New-Item -ItemType Directory -Force $OutDir | Out-Null

$argProj = 'NDK_PROJECT_PATH=' + $src
$argLibs = 'NDK_LIBS_OUT='     + (Join-Path $OutDir 'libs')
$argObj  = 'NDK_OUT='          + (Join-Path $OutDir 'obj')
& $ndkBuild -C (Join-Path $src 'jni') $argProj $argLibs $argObj APP_ABI=x86_64 APP_MODULES=modavd_gate 2>&1 |
  Out-String -Width 200
if ($LASTEXITCODE -ne 0) { throw "companion build failed (exit $LASTEXITCODE)" }

$so = Join-Path $OutDir 'libs\x86_64\libmodavd_gate.so'
Write-Output "OK $((Get-Item $so).Length) bytes  md5=$((Get-FileHash $so -Algorithm MD5).Hash.ToLower())"
Write-Output $so
