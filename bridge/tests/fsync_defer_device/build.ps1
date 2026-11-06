# Copyright 2026 qiyinxi. SPDX-License-Identifier: AGPL-3.0-or-later.
# Build the guest-side programs of the #35 / #96 on-device tests (x86_64 Android) with an NDK r27c:
#   fsync-defer-test        the module's unit test (bridge/tests/fsync_defer_test.cc)
#   eintr-probe             arms the real module in-process and storms the syncing thread with signals
#   eintr-probe-killable    the same, filter installed with SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV added
#   zscan                   lists runs of all-zero 4 KiB blocks in files
#   jank-probe              heartbeat / frame gaps / finished downloads / dirty pages on one timeline
#   tick                    prints the guest clock every 50 ms (host-vs-guest stall check, tick-host.ps1)
#   pwsh -File build.ps1 -Ndk <android-ndk-r27c> -Out <dir>
param([Parameter(Mandatory)][string]$Ndk, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$cxx = Join-Path $Ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe'
$here = $PSScriptRoot
$bridge = (Resolve-Path (Join-Path $here '..\..')).Path
New-Item -ItemType Directory -Force $Out | Out-Null
$base = @('--target=x86_64-linux-android30', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-static-libstdc++')
$noexc = @('-fno-exceptions', '-fno-rtti')
function Build([string]$name, [string[]]$pre, [string]$src, [string[]]$libs) {
    & $cxx @base @pre $src @libs -o (Join-Path $Out $name)
    if ($LASTEXITCODE -ne 0) { throw "$name failed to build" }
    "built $name"
}
Build 'fsync-defer-test' $noexc "$bridge\tests\fsync_defer_test.cc" @('-llog')
Build 'eintr-probe' ($noexc + "-I$bridge\companion\jni") "$here\eintr_probe.cc" @('-llog', '-ldl')
Build 'eintr-probe-killable' ($noexc + "-I$bridge\companion\jni" + '-DPROBE_KILLABLE') "$here\eintr_probe.cc" @('-llog', '-ldl')
Build 'zscan' @() "$here\zscan.cc" @()
Build 'jank-probe' @() "$here\jank_probe.cc" @()
Build 'tick' @() "$here\tick.cc" @()
