# Copyright 2026 qiyinxi
# SPDX-License-Identifier: AGPL-3.0-or-later (https://www.gnu.org/licenses/agpl-3.0.html)
#
# Build the native-bridge shim libmodavd_bridge.so (x86_64) with the NDK, from the
# sources in this repo (bridge\nbshim); outputs outside the source tree.
#
# NOTE: each make override must be ONE pre-built string argument — writing
# NDK_LIBS_OUT=(Join-Path ...) makes PowerShell emit two arguments and make then
# treats the path as a goal and silently builds nothing.
param(
  [string]$Ndk = $env:ANDROID_NDK_ROOT,
  [string]$OutDir   # default: %TEMP%\modavd-nbbridge\nbshim
)
$ErrorActionPreference = 'Stop'
if (-not $Ndk) { throw 'Android NDK not given: set ANDROID_NDK_ROOT to an NDK r27c, or pass -Ndk' }
$ndkBuild = Join-Path $Ndk 'ndk-build.cmd'
if (-not (Test-Path $ndkBuild)) { throw "ndk-build not found: $ndkBuild (pass -Ndk or set ANDROID_NDK_ROOT)" }
$tc   = Join-Path $Ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin'
$nb   = Split-Path -Parent $PSScriptRoot                 # bridge
$root = Join-Path $nb 'nbshim'
if (-not $OutDir) { $OutDir = Join-Path $env:TEMP 'modavd-nbbridge\nbshim' }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$argLibs = 'NDK_LIBS_OUT=' + (Join-Path $OutDir 'libs')
$argObj  = 'NDK_OUT='      + (Join-Path $OutDir 'obj')
$argProj = 'NDK_PROJECT_PATH=' + $root
$jni     = Join-Path $root 'jni'
& $ndkBuild -C $jni $argProj $argLibs $argObj APP_ABI=x86_64 APP_MODULES=modavd_bridge 2>&1 |
    Out-String -Width 200
if ($LASTEXITCODE -ne 0) { throw "nbshim build failed (exit $LASTEXITCODE)" }

$so = Join-Path $OutDir 'libs\x86_64\libmodavd_bridge.so'
if (-not (Test-Path $so)) { throw "nbshim build produced nothing at $so" }

# Gate: a dlopened library must carry no initial-exec TLS relocations, or the
# Bionic linker refuses it and the device loses all arm64 support.
$relocs = & (Join-Path $tc 'llvm-readelf.exe') -r $so 2>&1 | Out-String
if ($relocs -match 'TPOFF64|TLSDESC|DTPMOD64|DTPOFF64') {
    throw "REFUSING: $so carries TLS relocations. Remove thread_local / " +
          "-ftls-model=initial-exec. See Android.mk."
}

# Gate: NativeBridgeItf must be exported as a 168-byte GLOBAL OBJECT.
$syms = & (Join-Path $tc 'llvm-readelf.exe') --dyn-syms $so 2>&1 |
    Select-String -Pattern 'NativeBridgeItf'
if (-not $syms) { throw "REFUSING: $so does not export NativeBridgeItf" }

Write-Output "OK $((Get-Item $so).Length) bytes  md5=$((Get-FileHash $so -Algorithm MD5).Hash.ToLower())"
Write-Output "no TLS relocations; exported symbol:"
$syms | ForEach-Object { "  " + $_.Line.Trim() }
Write-Output "artifact: $so"
