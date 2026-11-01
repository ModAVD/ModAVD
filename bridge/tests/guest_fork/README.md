<!-- Copyright 2026 Ji Mao Cai -->
# ARM64 guest fork fixture

只在自己的独立、空闲 API 34 测试实例运行。包名固定为 `org.modavd.tests.guestfork`，APK 只含 ARM64 JNI 库；不向游戏注入代码。每次启动各做 3 轮 imported libc fork、显式 guest `SYS_clone(0x1200011)`、`SYS_clone(0x1300011)`，共 9 个 case。子进程只经 pipe 返回检查信息后退出，不返回 ART；raw tid 输出使用两侧 PROT_NONE 的独立 mmap 页。fixture 自身的 poll/wait 有期限，仍需外部超时限制启动和观察。

## 构建

需要 Python 3.9+、NDK r27c、JDK 21、API 34 的 android.jar、SDK build-tools 35。显式传所有工具位置，无 Gradle、下载或设备操作；Windows/Linux 使用相同 CLI（Linux 通常将 `python` 改为 `python3`）：

```text
python bridge/tests/guest_fork/build.py --ndk "<NDK>" --java-home "<JDK>" --android-jar "<platform-34>/android.jar" --build-tools "<build-tools-35>" --out bridge/tests/guest_fork/out
```

脚本自动选择 Windows/Linux NDK host 与可执行后缀，也可显式指定 `--ndk-host linux-x86_64` 等 prebuilt 目录名；本轮实际构建在 Windows，Linux 路径未实跑。C++ 使用 `aarch64-linux-android30`、C++17、O2、Wall/Wextra/Werror、no-exceptions/no-rtti；构建会验证签名、ELF64 AArch64 和 APK 中只有 ARM64 库。输出 APK、SO、manifest 与逐条构建日志；本地 debug 签名 key 只保留在输出目录。`out/`、签名 key 和生成物不得提交，Release 默认不构建或运行此 fixture。此脚本仅在显式调用时执行。

## 独立 ADB 验证

先安装要验证的伴生库并确认其哈希。在代码段填入自己的 ADB 路径、**私有端口**与测试实例 serial，再从仓库根目录运行。属性写入/恢复使用测试镜像已有的 `su 0` 权限，不执行 `adb root` 或重启设备。此示例会卸载/安装/停止**这个测试包**，不操作游戏；新生成的 debug key 与旧 APK 不同，因此先卸载旧测试包。`modavd.gate.target` 是替换名单，验证期间仅设为自有包，结束时必须恢复原值；不要与游戏回归并行。

下面是独立的 Python 验证示例，不是 build.py 的一部分。每条 ADB 命令有外部 30 秒 timeout，收集结果另有 30 秒预算；timeout/非零退出的阶段、命令和部分 stdout/stderr 会追加到 `validation-errors.jsonl`，cleanup 失败不会覆盖启动失败的证据。只收集同一 PID 的两个测试 tag，既不清空全局 logcat，也不把单纯 PASS 当路由证据。所有结果写入忽略的 out 目录。

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
print("PASS: ARM64, one PID, 9 functional cases, host-atfork=3, raw1200011=3, raw1300011=3")
```

验收必须联合功能与日志：imported fork 的 3 条独立 host-atfork held=1；显式 1200011 与 1300011 各 3 条 checker safe=1 和 raw held=1，并逐 case/round 关联同 PID。公共 `fork` 的 dladdr 在 guest libc 不代表其内部一定走 raw GOT。本 API 34 实物 proxy 的 `__clone_for_fork` thunk 转到宿主 Bionic fork；普通 imported fork 不能计为 raw GOT 覆盖。隔离 mmap 通过也不代表所有 guest pthread/tid 地址都通过保守 TLS/pthread/host-stack VMA 检查。

## 已有记录与边界

候选 `0015a574` 上已有 4 次严格运行，每次三路各 3 轮，共 36 个 case，通过功能与各路径 held=1 验收；当时测试 APK SHA256 为 `cac94012c2d0f32e55d19b8122b7894d76d2202c382e3ebec351b4f52a49654e`。本目录 C++/Java 与该 fixture 逐字节一致；XML 仅新增版权注释。通用脚本的新 APK 使用独立本地签名 key，构建哈希单列，不冒充已经运行过的旧 APK。

此前首次 `am start -W` 有一次 process attach timeout，窗口没有 fixture START/guard；后续诊断、严格重试与重复运行通过。起因仍未定位，不能宣称与 guard 绝对无关。外部 timeout 可能只结束等待它的宿主命令；示例 finally 会尝试停止自有测试包并恢复属性，恢复报错必须处理。不要在失败或只有 PASS 文本时宣称 raw 路由通过，也不要用 fixture 结果代替真实游戏或长期回归。
