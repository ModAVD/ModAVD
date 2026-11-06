# Copyright 2026 Ji Mao Cai
# SPDX-License-Identifier: GPL-2.0-only
"""Read-only download sampling on an explicitly selected ModAVD instance."""
import argparse
import hashlib
import json
import re
import subprocess
import time
from pathlib import Path


def run(argv, timeout=30):
    return subprocess.run([str(x) for x in argv], check=True, capture_output=True,
                          text=True, encoding="utf-8", errors="replace", timeout=timeout).stdout.strip()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--root", type=Path, required=True)
    p.add_argument("--index", type=int, required=True)
    p.add_argument("--package", required=True)
    p.add_argument("--expected-exe-sha256", required=True)
    p.add_argument("--build-label", required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--seconds", type=int, default=150)
    args = p.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_]+(?:\.[A-Za-z0-9_]+)+", args.package):
        p.error("Invalid package name")
    if args.seconds < 10 or args.seconds > 600:
        p.error("Sampling duration must be 10 through 600 seconds")
    config = json.loads((args.root / "mas-avd.json").read_text(encoding="utf-8-sig"))
    port = int(config.get("scriptAdbServerPort", 20049))
    if port == 5037:
        p.error("Default adb server is forbidden")
    info = json.loads(run([args.root / "tools/modavd.exe", "--root", args.root,
                           "info", args.index, "--json"]))["data"]
    if not info["running"] or not info["booted"] or info.get("busy"):
        raise RuntimeError("Selected instance is not ready")
    expected_avd = (args.root / "avd" / (info["name"] + ".avd")).resolve()
    if Path(info["path"]).resolve() != expected_avd:
        raise RuntimeError("Instance path does not belong to the requested root")
    exe = args.root / "sdk/emulator/qemu/windows-x86_64/qemu-system-x86_64.exe"
    process = json.loads(run(["powershell.exe", "-NoProfile", "-Command",
        f"Get-CimInstance Win32_Process -Filter 'ProcessId = {int(info['pid'])}' | "
        "Select-Object ExecutablePath | ConvertTo-Json -Compress"]))
    if not process or not process.get("ExecutablePath"):
        raise RuntimeError("Cannot verify the selected emulator process")
    if Path(process["ExecutablePath"]).resolve() != exe.resolve():
        raise RuntimeError("Running emulator executable differs from the requested root")
    if exe.stat().st_mtime * 1000 > info["started_at_ms"]:
        raise RuntimeError("Emulator executable changed after launch; restart before measuring")
    with exe.open("rb") as f:
        actual = hashlib.file_digest(f, "sha256").hexdigest()
    if actual != args.expected_exe_sha256.lower():
        raise RuntimeError("Emulator build does not match the expected executable")
    adb = args.root / "sdk/platform-tools/adb.exe"
    serial = info["adb_address"]
    run([adb, "-P", port, "connect", serial])
    prefix = [adb, "-P", port, "-s", serial, "shell"]
    script = '\n'.join([
        'echo "uptime=$(cat /proc/uptime)"',
        'echo "rx=$(cat /sys/class/net/eth0/statistics/rx_bytes)"',
        'echo "disk=$(cat /sys/block/vdc/stat)"',
        f'echo "pid=$(pidof {args.package})"',
        f'echo "external=$(du -sk /data/media/0/Android/data/{args.package}/files 2>/dev/null)"',
        f'echo "internal=$(du -sk /data/user/0/{args.package}/files 2>/dev/null)"',
        'echo "clock=$(cat /sys/devices/system/clocksource/clocksource0/current_clocksource)"',
        'echo "cache=$(cat /sys/block/vdc/queue/write_cache)"',
    ])
    def sample():
        raw = run(prefix + ["su 0 sh -c '" + script + "'"])
        fields = dict(line.split("=", 1) for line in raw.splitlines() if "=" in line)
        if not fields.get("pid"):
            raise RuntimeError("Game is not running; refusing an empty download measurement")
        disk = fields["disk"].split()
        return {"guest_seconds": float(fields["uptime"].split()[0]),
                "rx_bytes": int(fields["rx"]), "written_sectors": int(disk[6]),
                "external_kib": int(fields["external"].split()[0]) if fields["external"] else None,
                "internal_kib": int(fields["internal"].split()[0]) if fields["internal"] else None,
                "clocksource": fields["clock"], "write_cache": fields["cache"]}
    data = {"build_label": args.build_label, "emulator_sha256": actual,
            "package": args.package, "scope": "Observed installed build only; not candidate patch validation",
            "samples": []}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    begin = time.monotonic()
    while True:
        s = sample(); data["samples"].append(s)
        print(json.dumps(s), flush=True)
        args.out.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        elapsed = time.monotonic() - begin
        if elapsed >= args.seconds:
            break
        time.sleep(min(10, args.seconds-elapsed))
    first, last = data["samples"][0], data["samples"][-1]
    span = last["guest_seconds"] - first["guest_seconds"]
    if span <= 0 or last["rx_bytes"] < first["rx_bytes"]:
        raise RuntimeError("Guest restarted or network counter reset during measurement")
    data["summary"] = {"seconds": span,
        "network_mib_s": (last["rx_bytes"]-first["rx_bytes"])/span/1048576,
        "block_write_mib_s": (last["written_sectors"]-first["written_sectors"])*512/span/1048576}
    for key in ("external_kib", "internal_kib"):
        if first[key] is not None and last[key] is not None:
            data["summary"][key.replace("_kib", "_growth_mib_s")] = (last[key]-first[key])/span/1024
    args.out.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(data["summary"]), flush=True)


if __name__ == "__main__":
    main()
