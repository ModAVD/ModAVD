<!-- Copyright 2026 Ji Mao Cai. SPDX-License-Identifier: AGPL-3.0-or-later. -->
# Download batch callback test

The test includes the actual adapter, commit core, journal and listener
implementations. Link the control module separately:

```powershell
& '<NDK>\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe' --target=x86_64-linux-android30 -std=c++17 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -static-libstdc++ bridge\tests\download_batch_test.cc bridge\companion\jni\download_control.cc -llog -o '<output>\download-batch-test'
& '<adb>' -P <private-port> -s <disposable-instance> push '<output>\download-batch-test' /data/local/tmp/download-batch-test
& '<adb>' -P <private-port> -s <disposable-instance> shell chmod 755 /data/local/tmp/download-batch-test
& '<adb>' -P <private-port> -s <disposable-instance> shell su 0 /data/local/tmp/download-batch-test
```

The test accepts no paths. It creates a fresh `mkdtemp` tree under
`/data/local/tmp` on Android, or `/tmp` on Linux, and removes only its own files.
It uses real duplicate handles, writes, fsync, pathname copies, rename failures,
and durable journal operations. Controlled I/O callbacks inject EIO. Listener
health changes are injected into the actual listener's shared-state type.

Cases cover durable intent before ACK, journal failure, repeated requests on
one OFD, close/reopen of the same fd and inode, dup2 replacement, strict fallback
after close_range, namespace failure, flush failure, health loss, capacity,
foreign pointers, metadata hardlinks and an empty replacement journal. The
repeated-OFD case performs 64 requests with one path admission and one intent.
It now also requires exactly one `capture_checks` increment. A valid source
mapping reaches MarkDirty without another duplicate, fstat, readlink or close;
coordinated close/dup replacement invalidates that mapping, and every namespace
barrier removes it before any subsequent admission.

`capture_checks` counts owner new-source verification attempts, rather than the
core's independent lifetime duplicates. `negative_hits` counts requests forwarded
to real fsync through the bounded 32-source rejection cache. That cache never
ACKs. Close/dup replacement invalidates entries, every Drain clears the table,
and each new positive admission clears it again so a cached alias cannot consume
an owned OFD's writeback-error cursor. A full table falls back to full checks.
Tests cover deleted objects staying real, negative fd reuse, namespace changes
into the eligible scope, capacity, and replacement by a different target inode.
An execute-only parent-directory case drops a root test child to UID/GID 65534
and verifies search without parent listing while retaining a readable final
directory and rejecting a symlink.

The helper cases fork real processes and use the production shared control
module. They verify that owner debt is paid before the helper grant, owner ACKs
stay paused until the helper's real END, owner namespace operations can finish
during that pause, and a known helper rename failure permits fresh admission.
They also cover shared permanent restriction for a helper's foreign request and
denial of helper publication when the owner's real flush reports EIO. A completed
ordinary namespace operation does not permanently exclude an unfinished file;
its next request must pass full admission again.
The join cases require a fresh initialization sequence to reach GRANT and END
before publishing the helper role. A live owner process and READY page without
worker service must time out instead of admitting a helper. The production join
uses only the remainder of its total five-second budget for this probe; every
later real operation still has its own BEGIN/END.
Owner and helper callback cases also check io_uring setup/enter/register numbers
425/426/427: prior debt must be paid and future ACKs permanently disabled before
CONTINUE. These cases do not execute an io_uring syscall or weaken the platform's
original seccomp policy.

On 2026-11-07, the experiment controller ran all 28 callback cases on a
disposable Android instance using `su 0` with SELinux Enforcing. All passed.
The device log is `batch-test-fastpath-06.log` in the local experiment output.
That tested executable was built with NDK r27c and the command above; its
SHA-256 was
`dfab7a0919569d76e6bcdb08950e764bfd90444002eb419bf8b00d3c806378a2`.

The suite calls the production callback directly and installs no seccomp
filter. It does not test notification transport, the startup nonce/controller,
package-wide exclusion, guardian death, power loss or actual game downloads.
Those require separate installed-artifact and disposable-instance tests.
