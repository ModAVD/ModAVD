# arm64 execution bridge

[简体中文](README.md) | **English**

<!-- README.md and README.en.md match section by section: if you change one, change the other in the same PR. -->

The bridge lets games that only ship arm64 builds and are protected by a hardening library (the Onmyoji channel packages and the China-server build of Ruyuan) run on ModAVD's x86_64 system image. The bridge has shipped with the main Release package since 0.36.0 and is installed in `sdk\emulator\resources\modavd\`; when you start an instance with the ModAVD manager or the command line, it is installed into the instance automatically. The raw clone protection for the China-server build of Ruyuan is added by this PR, and its release starting point is the first Release that contains the change after it is merged; this page does not announce version numbers that have not been released yet.

**It does not modify, patch or bypass the game or its hardening library, and it does not interfere with any integrity or anti-tamper check.** This is compatibility work in the translation layer: it lets the hooks the game installs for itself execute and return correctly. Intel Houdini works the same way, and the same game runs on Houdini. The bridge itself does not change Google's translation layer. After boot, the manager additionally mounts a locally generated patched copy over the original path; it changes only the decoding of hint instructions (SEVL, SEV, WFE, WFI), leaves the image files untouched, and the modified binary is not distributed.

## Why it crashes

Google's native bridge keeps ART and the system libraries native x86_64 and translates only the app's own arm64 libraries, so one process is split in two halves.

To watch dex reads, the game's hardening library (arm64, translated) makes the host `libartbase.so` GNU_RELRO writable, overwrites the GOT slots of functions such as `read` and `mmap` with the address of its own arm64 hook, and then makes it read-only again. On a real device both sides are arm64, so everything works up to this point.

On an x86_64 image, native x86 ART executes `call [GOT slot]` and jumps into an arm64 code page. On the host side the translation layer maps guest code pages as non-executable, so the instruction fetch faults (`SIGSEGV`, `SEGV_ACCERR`, and the fault address is RIP). The translation layer's own signal handler has no branch for "native code jumped into a guest page" and treats it as a fatal error.

## Two libraries

| Directory | Output | What it does |
| --- | --- | --- |
| `nbshim/` | `libmodavd_bridge.so` | Our own native bridge entry point. `ro.dalvik.vm.native.bridge` points to it, and `libnativebridge` opens it and fetches `NativeBridgeItf`. It opens the real translation layer, copies the real struct over byte by byte using the length for its version (v6 = 144, v7 = 160, v8 = 168 bytes), forwards each entry, and brings up the companion library |
| `companion/` | `libmodavd_gate.so` | The bridge proper. Using the interfaces the translation layer itself exports (`WrapGuestFunctionImpl`, `RunGuestCall`, `GuestMapShadow`, `MakeTrampolineCallable`), it adds from the outside the missing path "native x86 calls a translated arm64 function through a rewritten GOT slot" |
| `nbshim/modavd_bridge.rc` | — | Re-registers the arm64 `binfmt_misc` for the new property value (the one that ships with the image recognizes only `libndk_translation.so`, so once the property changes, arm64 executables silently fail to run); loads zram at boot on instances that have been remounted |

Two generations of translation layer are supported: ndk_translation 0.2.3 on the API 34 image (this is what ModAVD uses) and Berberis 16 on API 36.1. Symbols are resolved by name from the already mapped images, not tied to a build-id.

## Only applies to apps on the list

- The list is a comma-separated set of package names compiled into the companion library (`kDefaultGateTargets` in `companion/jni/native_to_guest_companion.cc`). It contains the 7 Onmyoji channel packages and the Ruyuan China-server package `com.lingxigames.yuan.cn`; Ruyuan is on the list to enable the fork compatibility protection described below.
- If the property `modavd.gate.target` is set, it replaces the whole list; `setprop modavd.gate.target none` turns the bridge off.
- After a process is forked, for apps not on the list the forwarding callbacks and the slots the companion library modified are restored to their original state, so behavior is the same as without the bridge.

## Raw clone protection on API 34

The startup path of the Ruyuan China-server build goes through `CloneGuestThread` of the old translation layer: it turns a `CLONE_VM | CLONE_VFORK` request with an empty stack into a raw clone with a separate address space and calls the host kernel directly through `syscall`. This call does not go through Bionic `fork()`, so the existing `pthread_atfork` callbacks do not run; the child inherits translation layer mutexes held by other parent threads, while the parent thread sits in `kernel_clone` waiting for the child to exit. The investigation is in [Issue #20](https://github.com/ModAVD/ModAVD/issues/20).

The companion library also takes over the translation layer's own `syscall` GOT slot. Only a `SYS_clone` that has the fork protection enabled, does not carry `CLONE_VM`, and has an empty host stack can enter the protection; `CLONE_SETTLS` and `CLONE_PIDFD` always tail-jump to the original call at the assembly entry. `CLONE_PARENT_SETTID`, `CLONE_CHILD_SETTID` and `CLONE_CHILD_CLEARTID` can enter the protection: before the original clone, outside the loader and the three translation locks, the full 4-byte range of each active output is checked, and it must not intersect the VMA that holds the current host TP, `pthread_self()` or the stack marker. A range overflow, unreadable maps, or a VMA that cannot be identified as above all conservatively tail-jump; the private size of the host pthread is not guessed. This check rejects some host addresses that would be valid within the same VMA, and a guest output must also be classified by its actual mapping: it cannot skip the check just because it is named a guest pointer. Inactive tid arguments do not take part in the decision, and the original flags and pointers are not rewritten.

The pre-check saves and restores the entry errno; the naked entry saves the six GP arguments and RAX/AL, and restores the original RSP after the check returns. The unsafe branch then tail-jumps straight to the original syscall, so when the original clone returns it does not pass through the C++ epilogue of the companion library. When the protection is off it also tail-jumps directly.

On this API 34 image, two paths were observed separately. The public symbol of an ordinary ARM64 guest libc `fork()` belongs to `/system/lib64/arm64/libc.so`; its internal `__clone_for_fork` calls the host Bionic `fork()` through a thunk in Google's `libndk_translation_proxy_libc.so`, and the child then resets the current guest thread. The static call chain of the exact proxy is consistent with the independent host-atfork parent callback marker; this path uses the prepare/parent/child of `pthread_atfork` and does not use the raw clone loader callback. The dladdr ownership of the public fork does not by itself prove the internal split. An explicit guest `SYS_clone`, on the other hand, can go through the translation layer's syscall GOT: what is added here is the SETTID coverage for safe tid outputs on that path, including `0x1200011` (CHILD_SETTID, CHILD_CLEARTID, SIGCHLD) and `0x1300011` with PARENT_SETTID. A success of the ordinary imported fork must not be counted as raw GOT coverage, and results from this run cannot be extended to other images or other output addresses.

The three translation locks being protected belong to the default code pool, the TranslationCache, and the thread table needed for the child's reset. The offsets are enabled only for an API 34 translation layer whose build-id has been checked, and unknown versions do not use them. GOT installation is serialized within the public loader scope, with reentry, detach and already-installed states checked again; the original syscall value is published before the wrapper, and saving the wrapper itself as the original value is explicitly rejected.

Releasing locks alone is not enough: the fork snapshot can contain cache translations that other threads have not finished, and the child has no such worker, so it stays at a `Translating`, `Invalidating` or `Wrapping` entry forever. `InvalidateEntriesBeingTranslatedUnsafe` only changes `Translating` to `Invalidating` and clears the in-progress set; it still depends on the original worker to finish, so it cannot be used to recover a forked child.

The protection first confirms, under the three locks, that the cache is quiescent: the in-progress count must be 0, and no actual dispatch slot of the main map may be one of the three entries above. A busy lock and a cache Busy are returned separately; on Busy, the three locks taken this time are released first and the loader callback is exited, then it briefly nanosleeps to let the worker finish, and retries. If the structure or a stub cannot be judged, the result is Unknown: while holding the three locks, new guard admissions for this process are disabled permanently, and after leaving the locks and the loader a warning is logged; the current call immediately runs the original call unprotected, Unknown is not scanned again, and the guard is not re-enabled automatically. The owner/held state of an existing lock holder and initialized pointers are not rewritten by the disabling action. The read-only layout of the main map and the stub symbols are limited by the exact API 34 build-id `553ba82b76bb5a11fd1d67b2c7e01b75`, and instance addresses are cross-checked against the TranslationCache mutex; if a symbol is missing, the protection is not enabled. The host Bionic fork prepare uses the same full conditions. Neither parent nor child rewrites the cache; the child, including the timeout fallback branch, runs ForkChild to release the guard lock it inherited, and the parent branch releases only the locks this thread took. On release, held/owner are cleared before unlock, so the new state of the next lock holder is not cleared.

Another cold-start round also reproduced a child waiting in `dlopen` inside `ResetAllExecRegions` for an inherited linker mutex. The raw clone protection therefore first enters the public `dl_iterate_phdr`, uses the linker recursive lock it holds during the callback, and only inside the callback tries the three translation locks once, together with the cache check. On failure it immediately releases the translation locks it has taken and exits the callback, so translation threads waiting for the loader can continue, and then retries in the outer layer; only on success does it issue the original raw clone. Parent and child both exit the callback first and normally release the one extra layer of linker lock added this time, and only then run `ForkParent` / `ForkChild`. This subset keeps the original host cache tid, which satisfies the owner check of the linker's recursive unlock; it does not clear or rebuild the linker's internal locks. The ordinary Bionic `fork()` still uses the original `pthread_atfork` path and does not use this callback.

All other system calls are forwarded unchanged by an x86_64 assembly tail jump, preserving zero to six variadic arguments, the 64-bit return value and errno; a protected clone uses its original five arguments. For apps not on the list, all five GOT slots are restored. No Google binary or game file is modified, and neither the clone flags nor the arguments are changed. While the parent thread waits for the child during `CLONE_VFORK`, it still holds the linker and the three translation locks, so parent threads that need those locks pause; if the child gets stuck on some other resource that is not covered, the stall may extend to the whole process. The original 200 ms budget limits the retries for lock acquisition or cache Busy, and on timeout it logs a warning and runs the original call unprotected, so the original hang risk remains; waiting for the linker in the public `dl_iterate_phdr` is itself blocking, so the protection function as a whole has no hard 200 ms limit.

The quiescence check is an O(n) scan of the main map. On Ruyuan, with about 280,000 entries, the scan measured 15–25 ms; the tens of microseconds of an isolated set of 9,664 NotTranslated nodes cannot represent the cost in the game. By default only misses, Unknown, and slow calls where prepare plus protected reaches 100 ms are logged; setting `setprop modavd.gate.forkdiag 1` before the target process enables the protection turns on per-call diagnostics (default 0; after changing the property, a new process must be started to read it). The detailed log contains flags, the SETTID address and its host VMA classification, `cache_entries`, `scan_nodes`, `scan_us`, `wait_us` and `protected_us`, and is used to confirm that an explicit guest clone actually goes through the slot; the host callback is listed separately as `host fork atfork parent: held=...`, logged after unlock, in the parent branch only, preserving errno, and the raw guard does not produce this marker. scan_us is only the last scan; wait_us includes loader, lock acquisition, scan and retries. protected_us runs from after the successful scan to the release in the parent branch, includes the vfork child's running/waiting time, but does not include the scan time spent while already holding locks, so it is neither the time all four locks are held nor the net extra time of the patch.

For revision f6662a1, the Ruyuan Default run together with the 6-24 observations total 202 **completed** vfork guards, all with held=1; the P50 of protected_us is 28.820 ms, P95 is 35.4846 ms, and the maximum is 61.880 ms (linear interpolation by (n−1)×p, with the window including prepare/idle). These completed records cannot bound calls that did not complete or future child hangs. The longest visible gap in Default is about 9.131 s, with an estimated guest UTC of 05:02:47.442–05:02:56.573; within it there are indeed two short guards: wait/protected at 05:02:49.259 is 9.182/21.547 ms, and at 05:02:49.317 it is 9.005/23.879 ms. Their estimated prepare plus protected time totals about 63.613 ms, and no completed guard lasted 9.13 s; but the log is written after release, scheduling/log delay is unknown, and the clock pairing was taken later in the same boot, with an earlier clock calibration not independently ruled out, so it cannot be claimed that this long gap has nothing to do with the guard, or that indirect effects and unrecorded or unfinished calls are excluded.

For **f6662a1** the maintainer additionally ran a same-scenario regression on the Onmyoji official server: MaaYYs cold start, 60 s in the courtyard, and 3 Yuhun layer-1 battles passed, with battle fps of 57.2/57.7/57.1 against 55.8–57.4 fps for the official emulator, with no visible regression. Within 1 s after tapping to enter the game there were 15 vforks, with 365,000–370,000 cache entries; scan P50 20 ms / max 28 ms, wait P50 21 ms / P95 74 ms / max 155 ms, protected P50 25 ms / max 45 ms, all held=1, and misses/Unknown were 0; no guard occurred during battles. This is the maintainer's regression of an older external revision and cannot pass for a device result of the fix under review here; the 15 cold starts were still in progress at the time. See [the maintainer's record](https://github.com/ModAVD/ModAVD/pull/25#issuecomment-6542251111).

The native tests compile the actual implementation directly and cover zero-to-six-argument tail jumps, the 64-bit return value/errno, AL/original RSP/sixth stack argument after the unsafe tid pre-check, SETTLS/PIDFD pass-through, shared VM / non-empty host stack, and the fork-safety switch (it really toggles g_fork_safety; it is not the full package-name gating). SETTID coverage includes active/inactive pointers, NULL, aliasing, a 4-byte range across a VMA boundary / overflow, errno when maps is missing, the host TLS/pthread/stack of the main thread and workers, and a separate guest-like tid page with guard pages. A real-kernel SETTID fork verifies the output and parent/child COW; the native guest-like page and the separate mmap of the ARM fixture below each prove only their own output address, and do not mean that every guest pthread/tid address will pass the conservative VMA check.

They also cover: a timed-out child releasing a lock inherited from another thread's prepare, the parent keeping that owner, concurrent owner handover, Unknown disabling without rescanning, Busy backoff, rejection of the original syscall self-hook and concurrent publication of the original value, install reentry/detach, and the default and verbose diagnostic criteria. The three simulated translation states must wait for the worker to finish before the clone can proceed, and persistent Busy tests the original call/errno/no rewriting; a new set of 370,000 synthetic nodes with a 15-iteration serial burst has the last node Invalidating and inflight=0 each time, the worker needs the released cache lock to finish, and at the clone the whole tree is fully checked again. This is a synthetic functional/backoff stress test that includes extra verification, and it is neither game performance nor a real reproduction of vforks within 1 second. A real-kernel fork/vfork additionally checks that the parent and child caches are unchanged, that the child can use dliterate/dlopen again, the loader recursion level, and the reverse lock order. A passing build does not mean these runtime tests have passed; after compiling, run them on a test instance:

```powershell
& '<NDK>\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe' --target=x86_64-linux-android30 -std=c++17 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -static-libstdc++ bridge\tests\raw_clone_test.cc -llog -ldl -o '<输出>\raw-clone-test'
& '<adb>' -P <私有端口> -s <测试实例> push '<输出>\raw-clone-test' /data/local/tmp/modavd-raw-clone-test
& '<adb>' -P <私有端口> -s <测试实例> shell chmod 755 /data/local/tmp/modavd-raw-clone-test
& '<adb>' -P <私有端口> -s <测试实例> shell /data/local/tmp/modavd-raw-clone-test
```

The separate `cache_quiescence_test.cc` loads the real Google library in its own native process and only constructs metadata, without executing synthetic guest PCs. It verifies the real two-phase `NotTranslated → Translating → Invalidating → NotTranslated`, fork COW, Wrapping, explicitly asserts that inflight==0 for Wrapping, and checks acceptance/rejection at the same production entry. It only cross-confirms that GetInstance corresponds to the TranslationCache mutex `0x242fb8`; the other two actual lock offsets are uninitialized in the neutral process, so the test uses two locally initialized mutexes instead, and does not claim to have verified the runtime state of the code pool/thread table. The time for 9,664 ready nodes is recorded only as probe data for that layout. First confirm that the SHA-256 of `/system/lib64/libndk_translation.so` is `a2086fbc807f697f9b91222065c9d9000ca9727748699c0d9b751319258cbb3e`; the test also rejects a different build-id. The full-teardown API is used only in this separate test, and the production path does not call it.

```powershell
& '<NDK>\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe' --target=x86_64-linux-android30 -std=c++17 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -static-libstdc++ bridge\tests\cache_quiescence_test.cc -llog -ldl -o '<输出>\cache-quiescence-test'
& '<adb>' -P <私有端口> -s <测试实例> push '<输出>\cache-quiescence-test' /data/local/tmp/cache-quiescence-test
& '<adb>' -P <私有端口> -s <测试实例> shell chmod 755 /data/local/tmp/cache-quiescence-test
& '<adb>' -P <私有端口> -s <测试实例> shell /data/local/tmp/cache-quiescence-test translate
& '<adb>' -P <私有端口> -s <测试实例> shell /data/local/tmp/cache-quiescence-test wrap
```

The project's own ARM64-only fixture completed one strict three-path joint acceptance on the API 34 image above, with candidate `0015a574` (2026-10-31, PID 22824), passing 3 rounds of functional checks on each path. The ptid/ctid of the two raw paths are in separate mmap VMAs bordered by PROT_NONE on both sides; the log actually confirms safe=1, and the results of the ordinary imported fork cannot be used to make up these counts.

The text that can be reproduced publicly is in [`tests/guest_fork/`](tests/guest_fork/README.md): it contains the C++/Java as is, a manifest with only a copyright comment added, and a generic build CLI without Gradle. Its README gives explicit NDK/JDK/SDK paths, an external timeout, same-PID three-path round-by-round acceptance, and a standalone ADB example that temporarily overrides only the project's own test package and restores the property in a finally block. Three further strict repeats also passed, for a total of 4 launches with 3×3 each, 36 cases in all; these device results come from the old signed APK `cac94012` and cannot be relabeled as the new signed APK just generated by the generic script. The fixture is not part of the Release build or run by default.

| ARM64 fixture call | Observed protection path | Count this run |
| --- | --- | ---: |
| Public imported libc `fork()` | proxy → host Bionic → independent host-atfork parent held=1 | 3 |
| Explicit guest `SYS_clone(0x1200011)` | tid checker safe=1 → raw GOT guard held=1 | 3 |
| Explicit guest `SYS_clone(0x1300011)` | tid checker safe=1 → raw GOT guard held=1 | 3 |

The first `am start -W` hit one process attach timeout, and there was no fixture START/guard in that window; the later non-`-W` diagnostic launches and strict retries passed. The cause has not been located, so it cannot be claimed to be absolutely unrelated to this change, nor can the failure be attributed to the guard. The above are functional/routing results of the project's own fixture and do not replace Ruyuan or long-term regression.

Passing native tests must still be backed by real-game verification: Ruyuan cold start up to the resource update prompt, running the older games that are on the list, and restoring `5/5` for apps not on the list. A protocol page or a resource update prompt proves only that startup succeeded up to that stage, and cannot replace regression covering login, the courtyard, battles and long-term running.

## Signal order

The bridge works through SIGSEGV: the instruction fetch fault raised when native code jumps into a guest page through a rewritten GOT slot is the entry into the bridge. This fault must reach us first, otherwise another handler, such as the WebView crashpad, will treat it as a real crash.

Through the native bridge interface's `getSignalHandler(SIGSEGV)`, the shim hands the companion library's decision function to ART. ART registers it with libsigchain as a special handler, placed ahead of all `sigaction()` handlers. It recognizes only our own traps and lets everything else through.

## Build

You need Android NDK r27c.

```powershell
$env:ANDROID_NDK_ROOT = '<NDK r27c 的目录>'
pwsh -File bridge\scripts\build-companion.ps1 [-OutDir <目录>]
pwsh -File bridge\scripts\build-nbshim.ps1 [-OutDir <目录>]
```

- The outputs go under `%TEMP%\modavd-nbbridge\` by default.
- **Never add `-ftls-model=initial-exec`.** Both libraries are loaded with dlopen, and a library with initial-exec TLS relocations is rejected by the Bionic linker, so the whole native bridge fails to load and the device loses all arm64 support. `build-nbshim.ps1` checks after compiling and reports an error if there is such a relocation.
- When the same source is built in different directories, the outputs differ only in `.note.gnu.build-id` (the build-id includes the source path); with that section stripped they are byte-for-byte identical. So it is normal that the hashes of your own build do not match those in the Release; the SHA-256 of the three files in the Release is written in `modavd-manifest.json` at the root of the main package.

## Installing into an instance

- **Since 0.36.0 you do not need to install it yourself**: when you start an instance with the ModAVD manager `ModAVD.exe` or the command line `tools\modavd.exe start`, both new and existing instances get the bridge automatically: the boot marker is placed before boot, if the bridge is still not installed after the boot flow finishes it is installed, and if it does not take effect the instance is shut down and cold-started (no hot restart inside the guest), for at most three rounds. The first boot takes 1–2 minutes longer. After it is installed, just boot as usual (with the manager, the command line or AUTO-MAS); the bridge stays in place.
- An instance that has only ever been started with AUTO-MAS and never with the manager is not installed automatically in this version: start it once with the manager.
- If you do not want the bridge: create with `tools\modavd.exe create --no-bridge`, or run `tools\modavd.exe bridge <编号> uninstall` on an existing instance, after which it is no longer installed automatically on boot (run `install` again to restore it); `start --no-bridge` applies to that one start only. For a read-only check use `bridge <编号> verify` (the instance must be running).
- **Before going back to a version older than 0.36.0, run `tools\modavd.exe bridge <编号> uninstall` once on every instance**, and switch packages only after the result says it is not installed. If you go back without uninstalling first, that instance may end up with a system property pointing to a library that no longer exists, so no app that ships only an arm64 build will run, and nothing in the UI shows it.
- The one that actually does the work is `tools\modavd-bridge.ps1` in the package (its source is in [package/](../package/)); everything above calls it.

It takes care of:

- The instance must boot with a writable system image. Emulators from 0.36.0 on do this when they see the boot marker `modavd-writable-system` in the instance directory.
- The two `.so` files go into `/system/lib64` (the shim must be there: `libnativebridge` opens it in the linker's "system" namespace), and the `.rc` goes into `/system/etc/init/`.
- `ro.dalvik.vm.native.bridge=libmodavd_bridge.so` is appended to `/vendor/build.prop`. A backup is made before the change, the result is checked after writing and before the restart, and `sync` is run before the restart.
- The install order is files first and the property last; the uninstall order is the property first and then the files. The property is the only switch.

All of this is written into the instance's own system / vendor overlays and does not touch Google's original image.

## License

GNU Affero General Public License version 3 or later (AGPL-3.0-or-later); the full text is in [LICENSE](LICENSE) in this directory. Copyright 2026 qiyinxi.

The `NativeBridgeCallbacks` struct in `nbshim/jni/native_bridge_shim.cc` is declared following the layout of `native_bridge.h` of AOSP `libnativebridge` (Apache License 2.0), so that `libnativebridge` and the translation layer recognize it.
