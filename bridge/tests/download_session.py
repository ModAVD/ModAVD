# Copyright 2026 Ji Mao Cai. SPDX-License-Identifier: AGPL-3.0-or-later.
"""Arm one isolated download-sync experiment after stopping the whole package.

This is the external exclusivity boundary for the experimental native adapter.
It requires an explicitly staged test root, verifies its runtime, stops every
process of the package UID, consumes a fresh session token, then launches once.
It never deletes game files or operates through the default ADB server.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import stat
import subprocess
import time

PACKAGE = "com.hypergryph.arknights"
ACTIVITY = PACKAGE + "/com.u8.sdk.U8UnityContext"
MARKER = ".download-sync-test-root.json"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def physical(value):
    path = Path(os.path.abspath(value))
    for part in (path, *path.parents):
        if part.exists() or part.is_symlink():
            info = part.lstat()
            require(not stat.S_ISLNK(info.st_mode) and
                    not getattr(info, "st_file_attributes", 0) & 0x400,
                    "Test paths must not contain reparse points or symlinks")
    return path.resolve()


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run(argv, timeout=30):
    result = subprocess.run([str(arg) for arg in argv], check=True,
                            capture_output=True, text=True, encoding="utf-8",
                            errors="replace", timeout=timeout)
    return result.stdout.strip()


def load(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def context(root, index):
    root = physical(root)
    marker = load(root / MARKER)
    require(marker["experiment"] == "download-sync-v2" and marker["disposable"] is True,
            "Explicit disposable experiment marker required")
    require(physical(marker["root"]) == root and marker["index"] == index,
            "Marker identifies a different test instance")
    config = load(root / "mas-avd.json")
    sdk = physical(root / config["sdkRoot"])
    require(sdk == physical(marker["sdk"]), "Selected SDK differs from staged experiment")
    port = config["scriptAdbServerPort"]
    require(isinstance(port, int) and 1024 <= port <= 65535 and port != 5037,
            "An explicit private ADB port is required")
    cli = root / "tools/modavd.exe"
    require(digest(cli) == marker["manager_sha256"], "Manager executable changed")
    info = json.loads(run([cli, "--root", root, "--json", "info", index]))["data"]
    require(info["running"] and info["booted"] and not info.get("busy"),
            "Selected experiment must be running and idle")
    require(info["title"] == marker["title"] and info["index"] == index,
            "Instance title/index mismatch")
    expected_avd = physical(root / "avd" / (info["name"] + ".avd"))
    require(physical(info["path"]) == expected_avd, "AVD belongs to another root")
    serial = info["adb_address"]
    require(serial == "127.0.0.1:" + str(config["portBase"] + 2 * index + 1),
            "ADB address differs from the configured test port")
    pid = info["pid"]
    require(isinstance(pid, int) and pid > 0, "Invalid QEMU process")
    process = json.loads(run([
        "powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
        f"Get-CimInstance Win32_Process -Filter 'ProcessId={pid}' | "
        "Select-Object ExecutablePath,CommandLine | ConvertTo-Json -Compress",
    ]))
    exe = physical(process["ExecutablePath"])
    require(exe == physical(sdk / marker["qemu_relative"]), "Wrong running QEMU executable")
    require(digest(exe) == marker["qemu_sha256"], "Running QEMU hash changed")
    require("-no-window" in process["CommandLine"], "Use a headless disposable instance")
    discovery = Path(os.environ["LOCALAPPDATA"]) / "Temp/avd/running" / f"pid_{pid}.ini"
    fields = dict(line.split("=", 1) for line in
                  discovery.read_text(encoding="utf-8-sig").splitlines() if "=" in line)
    require(physical(fields["avd.dir"]) == expected_avd, "QEMU runtime AVD differs")
    adb = sdk / "platform-tools/adb.exe"
    require(digest(adb) == marker["adb_sha256"], "ADB executable changed")
    for name, expected in marker["bridge_sha256"].items():
        require(name in ("libmodavd_bridge.so", "libmodavd_gate.so", "modavd_bridge.rc"),
                "Unexpected bridge artifact")
        require(digest(sdk / "emulator/resources/modavd" / name) == expected,
                "Selected SDK bridge hash changed")
    run([adb, "-P", port, "connect", serial])

    def shell(command):
        return run([adb, "-P", port, "-s", serial, "shell", command])

    for name, expected in marker["bridge_sha256"].items():
        directory = "/system/etc/init/" if name.endswith(".rc") else "/system/lib64/"
        actual = shell("sha256sum " + directory + name).split()[0]
        require(actual == expected, "Installed guest bridge differs from the staged build")
    return root, info, marker, shell


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--index", type=int, default=0)
    parser.add_argument("--mode", choices=("off", "observe", "active"), required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--arm-only", action="store_true")
    args = parser.parse_args()
    evidence = physical(args.evidence)
    require(not evidence.exists(), "Refusing to overwrite session evidence")
    root, info, marker, shell = context(args.root, args.index)
    # A machine-wide lock is unnecessary: only this staged root may be operated.
    # O_EXCL also prevents two copies of this controller arming the same root.
    lock = root / ".download-session-controller.lock"
    descriptor = os.open(lock, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        os.write(descriptor, str(os.getpid()).encode("ascii"))
        os.fsync(descriptor)
        packages = shell("cmd package list packages -U " + PACKAGE)
        matches = re.findall(r"^package:" + re.escape(PACKAGE) + r" uid:(\d+)$",
                             packages, re.MULTILINE)
        require(len(matches) == 1, "Cannot identify the exact package UID")
        uid = int(matches[0])
        require(uid >= 10000, "Refusing a system UID")
        # Normal stop preserves ordinary data; recovery concerns only older
        # recorded download intents, never unsynchronized account/config data.
        shell("sync")
        shell("am force-stop " + PACKAGE)
        deadline = time.monotonic() + 15
        while True:
            processes = shell("ps -A -n -o UID,PID,NAME")
            remaining = [line for line in processes.splitlines()[1:]
                         if line.split() and line.split()[0] == str(uid)]
            if not remaining:
                break
            require(time.monotonic() < deadline,
                    "Package UID still has live processes; no session was armed")
            time.sleep(0.2)
        session = secrets.token_hex(16)
        shell("su 0 setprop modavd.download.sync " + args.mode)
        shell("su 0 setprop modavd.download.session " + session)
        require(shell("getprop modavd.download.sync") == args.mode and
                shell("getprop modavd.download.session") == session,
                "Failed to arm experiment properties")
        launched = ""
        if not args.arm_only:
            launched = shell("am start -n " + ACTIVITY)
            require("Error:" not in launched and "Exception" not in launched,
                    "Android rejected launch")
        evidence.parent.mkdir(parents=True, exist_ok=True)
        with evidence.open("x", encoding="utf-8") as stream:
            json.dump({"mode": args.mode, "session": session, "package_uid": uid,
                       "qemu_pid": info["pid"], "bridge_sha256": marker["bridge_sha256"],
                       "whole_uid_stopped": True, "launched": not args.arm_only,
                       "launch_result": launched}, stream, indent=2)
            stream.write("\n")
        print(json.dumps({"mode": args.mode, "whole_uid_stopped": True,
                          "launched": not args.arm_only}))
    finally:
        os.close(descriptor)
        lock.unlink()


if __name__ == "__main__":
    main()
