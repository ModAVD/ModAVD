<!-- Copyright 2026 Ji Mao Cai. SPDX-License-Identifier: AGPL-3.0-or-later. -->
# Download cache journal test

Build the test with the actual journal implementation, then run it on a
disposable Android instance using its configured private ADB port:

```powershell
& '<NDK>\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe' --target=x86_64-linux-android30 -std=c++17 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -static-libstdc++ bridge\tests\download_journal_test.cc bridge\companion\jni\download_journal.cc -o '<output>\download-journal-test'
& '<adb>' -P <private-port> -s <disposable-instance> push '<output>\download-journal-test' /data/local/tmp/download-journal-test
& '<adb>' -P <private-port> -s <disposable-instance> shell chmod 755 /data/local/tmp/download-journal-test
& '<adb>' -P <private-port> -s <disposable-instance> shell /data/local/tmp/download-journal-test
```

Linux builds use `c++ -std=c++17 -O2 -Wall -Wextra -Werror -fno-exceptions
-fno-rtti` with the same two source files. The program accepts no paths and
creates its own `mkdtemp` scratch tree under `/data/local/tmp` on Android or
`/tmp` on Linux. Cleanup never follows symlinks. No game directories are used.

On 2026-11-06, all 22 cases of the atomic-publication revision passed on the
x86_64 Android test instance as `su 0`, with SELinux Enforcing. In an earlier
ordinary ADB shell UID 2000 run, the first six cases passed, then hardlink setup
was denied by SELinux. The full shell-mode suite did not pass. No SELinux policy
or enforcing mode was changed.

The tests exercise real directory locks, file creation, fsync, safe traversal
and exact-key deletion, including a replacement inode at an admitted key.
They reject final/intermediate symlinks, hardlinks, non-regular targets,
malformed keys, bad checksums, partial records and duplicate logical keys.
Injected write/fsync errors cover intent creation, cache-parent persistence and
intent removal. Four admission cutpoints are exercised both with returned I/O
errors and with `fork` plus `_exit`: before writing, after a real partial write,
after file fsync, and after atomic rename but before directory fsync. Each case
keeps another previously acknowledged final intent and verifies it remains
recoverable. Interrupted cache recovery is also repeated with its file missing.

The directory lock serializes journal users; it does not stop the game. The
adapter must bind the private journal to the correct download root and exclude
all game access and namespace changes during startup recovery. Historical
dev/ino values are diagnostics, while the exact relative cache key owns recovery.
Admit also checks the current path against the captured identity. Retire requires
a real successful data fsync with no newer unpaid events.

Each `intent-00` through `intent-31` contains a 40-byte little-endian header
and an unterminated relative key. The header has the eight-byte magic
`MAVDJN01`, version 1, key length, diagnostic device/inode, CRC32 and a zero
reserved field. CRC32 covers the entire file with its checksum field zeroed.
Admission creates an exclusive `draft-00` through `draft-31`, writes the full
record, fsyncs it, then uses `renameat2(RENAME_NOREPLACE)` to publish the final
intent. Only a successful journal-directory fsync permits early ACK. Existing
final intents are never overwritten.

Startup validates every directory entry and final intent before deleting
anything. It may discard only exact, known draft names that are regular files,
have one link, belong to the current effective UID, have no group/other access
or owner execute bit, stay on the journal device and fit the record size bound.
It never reads a cache key from a draft or deletes that draft's cache object.
Unknown entries and corrupt/partial FINAL intents still block recovery before
any cache deletion and remain as evidence.

If cache-parent fsync fails, its intent is retained. If intent unlink succeeds
but journal-directory fsync fails, the session returns an error and blocks new
admissions. One attempt restores the intent through the same draft publication
protocol without overwriting an existing entry; neither successful restoration
nor a visible final path is guaranteed after that I/O failure. Close and
destruction release ownership without retiring debt.

These are native fault tests, not a power-loss test or proof of an adapter's
writer tracking, cache ownership or publication policy. They do not force a
cross-device mount beneath the scratch root. The implementation rejects changed
`st_dev` during traversal; this is not a claim to detect same-device bind mounts.
