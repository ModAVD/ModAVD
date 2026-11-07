# Copyright 2026 qiyinxi. SPDX-License-Identifier: AGPL-3.0-or-later.
"""Summarize ModAVD#96 download windows (dl-window.ps1 output) into one table.

For each <label>.guest.txt (in <dir>, or <dir>/windows): download rate from du growth (first to last du sample, MiB/s) and
network rx rate (MB/s, 10^6), from the guest's uptime stamps. For <label>-ts/ts-*.txt
(SurfaceFlinger timestats, 30 s each): the Arknights layer's frames, average fps, p99 and max
present-to-present interval, and frames with an interval >= 100 ms (a visible stall).
usage: python -I -X utf8 dl-sum.py <results dir>
"""
import re
import sys
from pathlib import Path


def window(guest: Path):
    samples = []
    cpus = []
    for line in guest.read_text(encoding="utf-8", errors="replace").splitlines():
        c = re.search(r"cpu=([\d,]+)", line)
        if c:
            cpus.append([int(x) for x in c.group(1).split(",")])
        m = re.search(r"sample i=(\d+) up=([\d.]+) du_kb=(\S+) rx=(\d+)", line)
        if m:
            du = None if m.group(3) == "-" else int(m.group(3))
            samples.append((float(m.group(2)), du, int(m.group(4))))
    du_pts = [(t, d) for t, d, _ in samples if d is not None]
    if len(du_pts) < 2:
        return None
    (t0, d0), (t1, d1) = du_pts[0], du_pts[-1]
    rx0, rx1 = samples[0][2], samples[-1][2]
    rt0, rt1 = samples[0][0], samples[-1][0]
    rates = []
    for (ta, da), (tb, db) in zip(du_pts, du_pts[1:]):
        rates.append((db - da) / 1024 / (tb - ta))
    cpu_busy = cpu_iow = "-"
    if len(cpus) >= 2:
        d = [b - a for a, b in zip(cpus[0], cpus[-1])]
        tot = sum(d)
        if tot:
            cpu_busy = round(100 * (tot - d[3] - d[4]) / tot)
            cpu_iow = round(100 * d[4] / tot)
    return {
        "cpu_busy": cpu_busy,
        "cpu_iow": cpu_iow,
        "secs": round(t1 - t0, 1),
        "du_mib_s": round((d1 - d0) / 1024 / (t1 - t0), 2),
        "rx_mb_s": round((rx1 - rx0) / 1e6 / (rt1 - rt0), 2),
        "du_rate_min": round(min(rates), 1),
        "du_rate_max": round(max(rates), 1),
    }


def frames(tsdir: Path):
    tot_frames, tot_ms, ge100, maxint = 0, 0, 0, 0
    hist = {}
    for f in sorted(tsdir.glob("ts-*.txt")):
        txt = f.read_text(encoding="utf-8", errors="replace")
        for block in txt.split("layerName = ")[1:]:
            name = block.splitlines()[0]
            if "arknights" not in name:
                continue
            m = re.search(r"present2present histogram is as below:\s*\n([^\n]+)", block)
            if not m:
                continue
            for a, b in re.findall(r"(\d+)ms=(\d+)", m.group(1)):
                a, b = int(a), int(b)
                if b:
                    hist[a] = hist.get(a, 0) + b
    if not hist:
        return None
    for ms, c in hist.items():
        tot_frames += c
        tot_ms += ms * c
        if ms >= 100:
            ge100 += c
        maxint = max(maxint, ms)
    acc, p99 = 0, 0
    for ms in sorted(hist):
        acc += hist[ms]
        if acc >= 0.99 * tot_frames:
            p99 = ms
            break
    return {"frames": tot_frames, "fps": round(1000 * tot_frames / tot_ms, 1) if tot_ms else 0,
            "p99_ms": p99, "max_ms": maxint, "ge100": ge100}


def main():
    res = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    wd = res / "windows" if (res / "windows").is_dir() else res
    rows = []
    for g in sorted(wd.glob("*.guest.txt")):
        label = g.name[: -len(".guest.txt")]
        w = window(g)
        if not w:
            continue
        fr = frames(wd / f"{label}-ts") or {}
        rows.append((label, w, fr))
    print("| 窗口 | 秒 | du MiB/s | 网络 MB/s | 每 30 s 段 MiB/s | 帧数 | 平均 fps | p99 帧间隔 ms | 最长帧间隔 ms | ≥100 ms 的帧 | 客体 CPU 忙 % | iowait % |")
    print("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
    for label, w, fr in rows:
        print(f"| {label} | {w['secs']} | {w['du_mib_s']} | {w['rx_mb_s']} | {w['du_rate_min']}–{w['du_rate_max']} | "
              f"{fr.get('frames', '-')} | {fr.get('fps', '-')} | {fr.get('p99_ms', '-')} | {fr.get('max_ms', '-')} | {fr.get('ge100', '-')} | {w['cpu_busy']} | {w['cpu_iow']} |")


if __name__ == "__main__":
    main()
