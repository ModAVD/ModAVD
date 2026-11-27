<!-- Copyright 2026 Ji Mao Cai -->
# ARM64 guest fork fixture

[简体中文](README.md) | **English**

<!-- README.md and README.en.md match section by section: if you change one, change the other in the same PR. -->

Run it only on your own separate, idle API 34 test instance. The package name is fixed as `org.modavd.tests.guestfork`, the APK contains only an ARM64 JNI library, and no code is injected into any game. Each launch runs 3 rounds each of an imported libc fork, an explicit guest `SYS_clone(0x1200011)` and `SYS_clone(0x1300011)`, 9 cases in all. The child only returns its check information through a pipe and then exits; it never returns into ART. The raw tid output uses a separate mmap page with PROT_NONE pages on both sides. The fixture's own poll/wait has a deadline, but you still need an external timeout to limit the launch and the observation.

## Build

You need Python 3.9+, NDK r27c, JDK 21, the API 34 android.jar and SDK build-tools 35. Pass every tool location explicitly; there is no Gradle, no download and no device operation. Windows and Linux use the same CLI (on Linux you usually change `python` to `python3`):

```text
python bridge/tests/guest_fork/build.py --ndk "<NDK>" --java-home "<JDK>" --android-jar "<platform-34>/android.jar" --build-tools "<build-tools-35>" --out bridge/tests/guest_fork/out
```

The script picks the Windows or Linux NDK host and the executable suffix by itself; you can also give the prebuilt directory name with `--ndk-host linux-x86_64` and the like. The build in this round was really done on Windows; the Linux path was not run. The C++ uses `aarch64-linux-android30`, C++17, O2, Wall/Wextra/Werror and no-exceptions/no-rtti. The build checks the signature, that the ELF is 64-bit AArch64, and that the APK holds only the ARM64 library. The output is the APK, the SO, the manifest and a log for every step; the local debug signing key stays in the output directory only. Do not commit `out/`, the signing key or the generated files. The Release does not build or run this fixture by default, and the script runs only when you call it explicitly.

## Standalone ADB check

First install the companion library you want to verify and confirm its hash. Fill in your own ADB path, **private port** and test instance serial in the code block, then run it from the repository root. Writing and restoring the properties uses the `su 0` permission the test image already has; it does not run `adb root` or restart the device. This example uninstalls, installs and stops **this test package** and does not touch any game; the newly generated debug key differs from the old APK's, so the old test package is uninstalled first. `modavd.gate.target` is the replacement list: during the check it is set to your own package only and must be restored at the end. Do not run this in parallel with a game regression.

The example below is a standalone Python check and not part of build.py. Every ADB command has an external 30-second timeout, and collecting the result has another 30-second budget; for a timeout or a non-zero exit, the phase, the command and the partial stdout/stderr are appended to `validation-errors.jsonl`, and a cleanup failure does not overwrite the evidence of a launch failure. It collects only the two test tags of the same PID: it does not clear the global logcat, and it does not take a bare PASS line as proof of the route. All results are written to the ignored out directory.

```python
from pathlib import Path
import json
import re
import shlex
import subprocess
import time

ADB = ["<adb>", "-P", "<private-port>", "-s", "<test-serial>"]
PKG = "org.modavd.tests.guestfork"
OUT = Path("bridge/tests/guest_fork/out")
OUT.mkdir(parents=True, exist_ok=True)
phase = "read-properties"

def text(value):
    return value.decode("utf-8", "replace") if isinstance(value, bytes) else (value or "")

def record_failure(command, kind, stdout, stderr, **details):
    record = {"timeNs": time.time_ns(), "phase": phase, "command": command,
              "kind": kind, "stdout": text(stdout), "stderr": text(stderr), **details}
    with (OUT / "validation-errors.jsonl").open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(record, ensure_ascii=False) + "\n")

def adb(*args, check=True, timeout=30):
    command = ADB + list(args)
    try:
        result = subprocess.run(command, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        record_failure(command, "timeout", error.stdout, error.stderr, timeoutSeconds=timeout)
        raise
    if result.returncode:
        record_failure(command, "exit", result.stdout, result.stderr, returncode=result.returncode)
        if check:
            result.check_returncode()
    return text(result.stdout)

def shell(*args, **kwargs):
    return adb("shell", shlex.join(args), **kwargs).strip()

saved = {name: shell("getprop", name) for name in
         ["modavd.gate.target", "modavd.gate.forkdiag"]}
try:
    phase = "setup"
    shell("su", "0", "setprop", "modavd.gate.target", PKG)
    shell("su", "0", "setprop", "modavd.gate.forkdiag", "1")
    adb("uninstall", PKG, check=False)
    adb("install", "-r", str(OUT / "guest-fork-probe.apk"))
    assert "primaryCpuAbi=arm64-v8a" in shell("dumpsys", "package", PKG)
    # Take a device log-clock cursor before launch; do not reuse old PID records.
    tail = adb("logcat", "-b", "main", "-d", "-v", "threadtime", "-t", "1")
    cursors = re.findall(r"^\d\d-\d\d \d\d:\d\d:\d\d\.\d{3}", tail, re.M)
    assert cursors, "could not obtain a pre-launch log cursor"
    cursor = cursors[-1]
    phase = "launch"
    shell("am", "start", "-W", "-n", PKG + "/.ProbeActivity")
    phase = "collect"
    deadline = time.monotonic() + 30
    log = ""
    pid = ""
    while time.monotonic() < deadline:
        budget = lambda: max(0.1, min(5, deadline - time.monotonic()))
        pids = shell("pidof", PKG, timeout=budget()).split()
        if len(pids) == 1:
            pid = pids[0]
            log = adb("logcat", "-b", "main", "-d", "-T", cursor, "--pid=" + pid, "-v", "threadtime",
                      "ModAvdGate:I", "ModavdGuestForkProbe:I", "*:S", timeout=budget())
            (OUT / "validation.log").write_text(log, encoding="utf-8")
            if "PASS ARM64 imported fork" in log or "FAIL " in log:
                break
        time.sleep(0.1)
    lines = log.splitlines()
    phase = "verify"
    starts = [i for i, line in enumerate(lines) if "START hostPid=" + pid in line]
    assert starts, "missing fixture START; retain launch/AMS evidence"
    log = "\n".join(lines[starts[-1]:])
    assert "PASS ARM64 imported fork and raw 1200011/1300011, 3 rounds each" in log
    cases = re.findall(r"case=(\S+) round=(\d+) result=(-?\d+) pid=(\d+)", log)
    names = {"imported-libc-fork", "raw-bionic-flags", "raw-parent-child-settid"}
    assert len(cases) == 9 and {(name, int(r)) for name, r, _, _ in cases} == {
        (name, r) for name in names for r in range(3)}
    assert all(code == "0" and case_pid == pid for _, _, code, case_pid in cases)
    assert log.count("host fork atfork parent: held=1") == 3
    # Every imported fork round passed the loader guard: one marker before its case line.
    log_lines, previous, guarded = log.splitlines(), 0, 0
    for index, line in enumerate(log_lines):
        if "case=" not in line:
            continue
        window, previous = log_lines[previous:index], index + 1
        if "case=imported-libc-fork " in line:
            guarded += sum(bool(re.search(r"host fork loader guard: result=[1-9]\d* ", w)) for w in window) == 1
    assert guarded == 3
    for flags in ["1200011", "1300011"]:
        assert log.count("raw clone tid outputs: flags=" + flags + " safe=1 ") == 3
        assert len(re.findall(r"raw clone guard: flags=" + flags +
                              r" held=1 result=[1-9]\d* ", log)) == 3
finally:
    phase = "cleanup"
    cleanup_errors = []
    for command in [("am", "force-stop", PKG)] + [
            ("su", "0", "setprop", name, value) for name, value in saved.items()]:
        try:
            shell(*command)
        except Exception as error:
            print("Cleanup/restore failed; restore the saved property manually:", command, error)
            cleanup_errors.append(error)
    if cleanup_errors:
        raise RuntimeError("test package/property cleanup incomplete")
print("PASS: ARM64, one PID, 9 functional cases, host-atfork=3, loader-guard=3, raw1200011=3, raw1300011=3")
```

The acceptance combines function and log: the imported fork has 3 separate host-atfork held=1 lines, and each round has exactly one `host fork loader guard: result=<child>` line before its case line (the host fork is issued under the loader lock, see [bridge/README](../../README.en.md)); the explicit 1200011 and 1300011 each have 3 checker safe=1 lines and 3 raw held=1 lines, matched case by case and round by round on the same PID. The dladdr of the public `fork` does not mean the guest libc's internal path goes through the raw GOT. On this API 34 real-device proxy, the `__clone_for_fork` thunk forwards to the host Bionic fork, so an ordinary imported fork cannot be counted as raw GOT coverage. Passing on the isolated mmap does not mean that every guest pthread/tid address passes the conservative TLS/pthread/host-stack VMA check either.

## Existing records and limits

On candidate `0015a574` there have been 4 strict runs, each with 3 rounds on each of the three routes, 36 cases in all, and they passed the function and per-route held=1 acceptance; the test APK SHA256 at that time was `cac94012c2d0f32e55d19b8122b7894d76d2202c382e3ebec351b4f52a49654e`. The C++/Java in this directory is byte for byte the same as that fixture; the XML only gained a copyright comment. The new APK from the generic script uses a separate local signing key and its build hash is listed on its own; it does not stand in for the old APK that was actually run.

Earlier, the first `am start -W` once hit a process attach timeout, and that window had no fixture START/guard; the later diagnostic runs, strict retries and repeated runs passed. The cause is still not located, so do not claim it is certainly unrelated to the guard. An external timeout may end only the host command that waits for it; the example's finally block tries to stop the own test package and restore the properties, and a restore error must be handled. Do not claim the raw route passed when it failed or when there is only PASS text, and do not use fixture results in place of a real game or a long-term regression.
