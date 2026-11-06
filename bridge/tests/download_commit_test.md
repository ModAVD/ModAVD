<!-- Copyright 2026 Ji Mao Cai. SPDX-License-Identifier: AGPL-3.0-or-later. -->
# Download commit core test

From the repository root, build the test and the actual core together:

```powershell
& '<NDK>\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe' --target=x86_64-linux-android30 -std=c++17 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -static-libstdc++ bridge\tests\download_commit_test.cc bridge\companion\jni\download_commit.cc -o '<output>\download-commit-test'
& '<adb>' -P <private-port> -s <disposable-instance> push '<output>\download-commit-test' /data/local/tmp/download-commit-test
& '<adb>' -P <private-port> -s <disposable-instance> shell chmod 755 /data/local/tmp/download-commit-test
& '<adb>' -P <private-port> -s <disposable-instance> shell /data/local/tmp/download-commit-test
```

The program accepts no paths. POSIX cases create a fresh `mkdtemp` directory
under `/data/local/tmp` on Android or `/tmp` on Linux, then remove only their
own files. They exercise real duplicate handles, fd reuse, writes, fsync and
readback. EIO and ENOSPC are injected as worker completion results; the test
does not force filesystem exhaustion or simulate a power loss.

On Linux, compile both sources with `c++ -std=c++17 -O2 -Wall -Wextra -Werror`.
From a Visual Studio developer shell, the same sources build with
`cl /std:c++17 /W4 /WX /EHsc`. Windows runs the fake-handle cases and reports
the POSIX cases as skipped.

These tests cover ownership and generation transitions. They do not establish
that a downloader observes every writer, safely publishes a resource, or
recovers after a crash. Readback after fsync is not a power-loss test.
