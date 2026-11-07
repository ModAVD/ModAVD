<!-- Copyright 2026 Ji Mao Cai. SPDX-License-Identifier: AGPL-3.0-or-later. -->
# Download process control test

Build the test with the actual controller, then run on an isolated Android
instance using its configured private ADB port:

```powershell
& '<NDK>\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe' --target=x86_64-linux-android30 -std=c++17 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -static-libstdc++ bridge\tests\download_control_test.cc bridge\companion\jni\download_control.cc -o '<output>\download-control-test'
& '<adb>' -P <private-port> -s <disposable-instance> push '<output>\download-control-test' /data/local/tmp/download-control-test
& '<adb>' -P <private-port> -s <disposable-instance> shell chmod 755 /data/local/tmp/download-control-test
& '<adb>' -P <private-port> -s <disposable-instance> shell su 0 /data/local/tmp/download-control-test
```

On 2026-11-07, all 18 cases passed on the x86_64 Android test instance as
`su 0`, with SELinux Enforcing. The NDK build used the flags above and its
SHA256 was `4e076e117b5fcc491c5ecfa9baf39790a60c5c6308031a9c94b5505c7d0ee0b2`.
The device log is `control-test-pidfd-03.log` in the local experiment output.
No game directories or device policy were changed by this program.

The parent creates its own `mkdtemp` directory under `/data/local/tmp` on
Android or `/tmp` on Linux. Helpers start through fresh `exec`, open the same
control files independently and perform real writes/fsync or a real failing
rename in that scratch directory. The owner calls `Service` in an idle loop;
its drain callback performs real fsync. An injected callback EIO denies the
grant. The fixture polls every 2 ms; wiring the listener's 100 ms idle Tick is
an adapter integration check, not something this standalone program proves.

Coverage includes helper serialization while the owner never takes their
lock, normal completion permitting fresh admission, known namespace failure,
Permanent-only handoff, timeout during a slow drain, late END, stale session
nonces, owner death without election, and helper death after GRANT or during
EXECUTING. GRANT death uses SIGSTOP before granting, then SIGKILL. A separate
fork fixture closes an inherited active helper object and verifies a third
fresh-exec helper remains blocked until the original helper ends its ticket.

Timeouts and slow drains are deliberate fixtures. The tests do not force PID
reuse, simulate a power loss, prove UID-wide startup exclusion, or establish
that the game adapter covers every sync/namespace syscall. They do not run a
real seccomp CONTINUE handoff. Debt retirement and map invalidation remain the
owner drain callback's obligations; their implementations have separate tests.

The owner holds an independent flock on `control.page` for its lifetime.
Helpers hold a separate `control.lock` from BEGIN through END/handoff. Both
files are bridge-owned and may coexist with other files in the wrapper
directory. Shared lock-free atomic state combines a monotonically increasing
sequence with its phase. The page also carries its version, trusted session
nonce, owner/helper PID plus self-reported start ticks, deadline and errors.
Only `/proc/self/stat` is read for identity diagnostics. Cross-process liveness
uses held pidfds and zero-timeout `ppoll`; unreadable peer `/proc` paths are
never treated as death. A helper holds its captured owner pidfd until Close;
the owner holds a helper pidfd for that request sequence until END. Capability
and permission errors keep their original errno. No raw `SYS_poll` is used.

Opening a page and capturing a live pidfd do not authorize execution. Each real
operation requires a fresh sequence's worker-serviced drain and GRANT. An
initial PID-number reuse can therefore only wait for a grant that never comes,
not consume an earlier ticket. The adapter must also complete a fresh empty
BEGIN/END probe before announcing initial helper join. No owner replacement is
elected if that handshake fails.

Only the owner worker may publish startup readiness or serve drain callbacks.
Normal END reports an actual completion; known namespace errno does not itself
disable future admission. Permanent `FinishHandoff` releases coordination
without asserting syscall success or completion and can never reopen early
ACKs. Timeout, death or uncertainty poisons the session. A fresh nonce after
the controller stops the entire UID is required to reset it; there is no
same-session owner election or file recovery in this module.

Control-page layout version 2 publishes its header under a monotonically
increasing 64-bit generation. An odd generation means initialization; a reader
accepts only identical even generations surrounding all atomic header loads.
The helper repeats that snapshot after checking owner liveness and treats any
change as Busy. Reset uses atomic stores, so a reused page cannot expose a
new nonce with the previous owner's PID/start identity through a magic-value ABA.

The initialization race fixture repeats eight fresh sessions. At a deterministic
cutpoint after writing the new nonce but before replacing owner identity, a
separately exec'ed helper must return Busy. The first previous owner was killed
and reaped, which would make a mixed header report a false death. After each
publication another helper completes a normal barrier against the new owner.

The non-dumpable fixture separately execs an owner and a helper, sets each test
child's `PR_SET_DUMPABLE` to zero, and completes a real barrier/write/fsync/END.
It does not make any production process dumpable.

The separate `listener-app-device-04-uring` Instrumentation run passed on the
same date in `untrusted_app`, without
su, namespace changes or policy changes. Its original dumpable value was already
zero. In both directions between same-UID processes, peer `/proc/PID/stat`
returned ENOENT while own stat remained readable. `pidfd_open` succeeded,
`ppoll` returned zero while the peer lived, then returned POLLIN after the peer
exited. This reproduces the old controller's false-death mechanism and validates
the replacement primitives under the app syscall allowlist. It does not prove
the full game cohort's join/barrier integration or download performance; those
still require the rebuilt adapter's actual game run.
