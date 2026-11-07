# Copyright 2026 qiyinxi. SPDX-License-Identifier: AGPL-3.0-or-later.
# #96 mutation check: build one fsync-defer-test per mutant of fsync_defer.cc, without touching the worktree.
# Each mutant is a copy of bridge/companion/jni/fsync_defer.cc with ONE textual change, compiled with the
# unchanged bridge/tests/fsync_defer_test.cc (copied next to it so its relative #include finds the mutant).
# Every mutant must make the on-device run print "N FAILURE(S)"; "ALL PASS" means the tests miss that bug.
param(
    [string]$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path,
    [Parameter(Mandatory)][string]$Out,
    [Parameter(Mandatory)][string]$Ndk   # NDK r27c
)
$ErrorActionPreference = 'Stop'
$Cxx = Join-Path $Ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe'
$src = Get-Content -Raw -Encoding utf8 (Join-Path $Repo 'bridge\companion\jni\fsync_defer.cc')
$test = Join-Path $Repo 'bridge\tests\fsync_defer_test.cc'

# name -> list of @(old, new) replacements (each old must occur exactly once)
$mutants = [ordered]@{
    # earlier round (11-05 00:0x), re-run against the extended test
    'A-flush-never'     = @(, @("    if (deferred == 0 || deferred <= paid) continue;  // nothing unpaid`n    const int fd = g_matched[i].payback_fd", "    (void)paid; if (deferred == 0 || true) continue;  // nothing unpaid`n    const int fd = g_matched[i].payback_fd"))
    'B-shutdown-unset'  = @(, @('g_shutdown.store(true, std::memory_order_release);  // (3)', 'g_shutdown.store(false, std::memory_order_release);  // (3)'))
    'C-guard-removed'   = @(, @("  if (!g_installed.load(std::memory_order_acquire)) return;`n  g_shutdown", "  if (false) return;`n  g_shutdown"))
    'D-defer-observe'   = @(, @('return (g_mode == kActive) && DecideDefer(req);', 'return (g_mode == kObserve) && DecideDefer(req);'))
    # new: the deferral decision
    'I-no-devino-check' = @(, @("  if (e.dev.load(std::memory_order_relaxed) != st.st_dev ||`n      e.ino.load(std::memory_order_relaxed) != st.st_ino)`n    return false;`n  const int idx", "  const int idx"))
    'J-no-foreign-check'= @(, @("  if (access(taskp, F_OK) != 0) return false;`n  // fd-number reuse", "`n  // fd-number reuse"))
    'O-observe-takes-slot' = @(, @('(g_mode == kActive)
                        ? MatchedAdd(', '(true)
                        ? MatchedAdd('))
    # new: the payback fsync
    'E-no-throttle'     = @(, @("  if (now - paid < kPaybackEveryNs) return false;        // paid recently enough`n", "`n"))
    'F-payback-no-magic'= @(, @('  const long r = syscall(__NR_fsync, fd, static_cast<long>(kMagic));', '  const long r = syscall(__NR_fsync, fd, 0L);'))
    'G-payback-game-fd' = @(, @("  int fd = g_matched[i].payback_fd.load(std::memory_order_relaxed);`n  bool opened = false;", "  int fd = g_matched[i].fd;`n  bool opened = false;"))
    'H-flush-skips-fsync' = @(, @("    const long r = PayBackHandle(fd);`n    g_matched[i].paid_ns.store(NowNs()", "    const long r = 0;`n    g_matched[i].paid_ns.store(NowNs()"))
    'K-retire-unpaid'   = @(@("    if (deferred > paid) return;         // an unpaid tail remains", "    (void)deferred;"),
                            @('matched != 0 && deferred <= paid &&', 'matched != 0 && (deferred != -12345) &&'))
    'M-recv-enoent-uncounted' = @(, @("    g_c_recv_enoent.fetch_add(1, std::memory_order_relaxed);`n    return false;", "    return false;"))
    'P-bpf-not-only-fsync' = @(, @('BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kNrRenameat2, 4, 5),', 'BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kNrRenameat2, 4, 4),'))
    'R1-rename-no-payback' = @(, @("  if (!unpaid) return false;`n  FlushAllDeferred();`n  return true;", "  (void)unpaid;`n  return false;"))
    'R2-bpf-rename-not-trapped' = @(, @('BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kNrRename, 6, 0),', 'BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kNrRename, 7, 0),'))
    'R3-rename-payback-in-observe' = @(, @("bool PayBackBeforeRename(const struct seccomp_notif* req) {`n  if (g_mode != kActive) return false;", "bool PayBackBeforeRename(const struct seccomp_notif* req) {"))
    'R4-rename-payback-foreign' = @(, @("  if (access(taskp, F_OK) != 0) return false;`n  bool unpaid = false;", "  bool unpaid = false;"))
    'S1-safe-no-page-floor' = @(, @('"%lld", len & kPageMask);', '"%lld", len);'))
    'S2-payback-no-safe-length' = @(, @('  if (r == 0) RecordSafeLength(fd, s0);', '  if (r == 0) RecordSafeLength(fd, -1 - 0 * s0);'))
    'S4-repair-no-cut' = @(, @('if (safe >= 0 && size > safe && ftruncate(fd, safe) == 0) {', 'if (false && safe >= 0 && size > safe && ftruncate(fd, safe) == 0) {'))
    'S5-repair-ignores-marker' = @(, @('  const ssize_t n = fgetxattr(fd, kSafeXattr, buf, sizeof(buf) - 1);', '  const ssize_t n = 1; buf[0] = ''0'';'))
    'S7-match-no-payback' = @(, @('      if (handle >= 0) PayBackHandle(handle);  // safe length from the start (crash repair)', '      (void)handle;'))
    'S8-repair-ignores-other-processes' = @(, @('      found = static_cast<pid_t>(pid);', '      found = 0 * static_cast<pid_t>(pid);'))
    'L-bpf-magic-trapped' =@(, @('BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kMagic, 1, 0),', 'BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kMagic, 0, 0),'))
}

New-Item -ItemType Directory -Force $Out | Out-Null
foreach ($name in $mutants.Keys) {
    $m = $src
    foreach ($pair in $mutants[$name]) {
        $old = $pair[0] -replace "`r`n", "`n"; $new = $pair[1] -replace "`r`n", "`n"
        $mm = $m -replace "`r`n", "`n"
        $count = ([regex]::Matches($mm, [regex]::Escape($old))).Count
        if ($count -ne 1) { throw "$name : expected 1 match, found $count for: $old" }
        $m = $mm.Replace($old, $new)
    }
    $dir = Join-Path $Out "src-$name"
    New-Item -ItemType Directory -Force (Join-Path $dir 'companion\jni'), (Join-Path $dir 'tests') | Out-Null
    [IO.File]::WriteAllText((Join-Path $dir 'companion\jni\fsync_defer.cc'), $m, [Text.UTF8Encoding]::new($false))
    Copy-Item $test (Join-Path $dir 'tests\fsync_defer_test.cc') -Force
    $bin = Join-Path $Out "fsync-defer-test-$name"
    & $Cxx --target=x86_64-linux-android30 -std=c++17 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -static-libstdc++ `
        (Join-Path $dir 'tests\fsync_defer_test.cc') -llog -o $bin 2>&1 | Out-String | ForEach-Object { if ($_.Trim()) { "$name : $_" } }
    if ($LASTEXITCODE -ne 0) { throw "$name failed to compile" }
    "$name built"
}
