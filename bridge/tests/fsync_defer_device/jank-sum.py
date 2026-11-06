# Copyright 2026 qiyinxi. SPDX-License-Identifier: AGPL-3.0-or-later.
"""Correlate jank-probe output (windows/<label>.jank.txt): for every frame gap > 100 ms, is there a
*.downloading file that finished within +-2 s, and a heartbeat (whole-guest) gap overlapping it?
usage: python -I -X utf8 jank-sum.py <jank.txt>...
"""
import re
import sys


def load(path):
    ev = {"FRAME": [], "HB": [], "DONE": [], "NEW": [], "MEM": []}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"\s*(\d+) (FRAME gap|HB gap) (\d+) ms", line)
        if m:
            ev["FRAME" if m.group(2).startswith("FRAME") else "HB"].append((int(m.group(1)), int(m.group(3))))
            continue
        m = re.match(r"\s*(\d+) DL (done|new)", line)
        if m:
            ev["DONE" if m.group(2) == "done" else "NEW"].append(int(m.group(1)))
            continue
        m = re.match(r"\s*(\d+) MEM dirty=(\d+)MB writeback=(\d+)MB blocked=(\d+)", line)
        if m:
            ev["MEM"].append(tuple(int(x) for x in m.groups()))
    return ev


for path in sys.argv[1:]:
    ev = load(path)
    frames = ev["FRAME"]
    near_done = sum(1 for t, d in frames if any(-2000 <= t - x <= 2000 + d for x in ev["DONE"]))
    with_hb = sum(1 for t, d in frames if any(h <= t + d and t <= h + hd for h, hd in ev["HB"]))
    big = [(t, d) for t, d in frames if d >= 300]
    big_near = sum(1 for t, d in big if any(-2000 <= t - x <= 2000 + d for x in ev["DONE"]))
    peak_dirty = max((m[1] + m[2] for m in ev["MEM"]), default=0)
    blocked = max((m[3] for m in ev["MEM"]), default=0)
    print(f"{path}: frame gaps>100ms={len(frames)} (>=300ms: {len(big)}), files done={len(ev['DONE'])}, "
          f"heartbeat gaps>50ms={len(ev['HB'])} (max {max((d for _, d in ev['HB']), default=0)} ms)")
    print(f"  frame gaps within 2 s of a file finishing: {near_done}/{len(frames)} (>=300ms: {big_near}/{len(big)}); "
          f"overlapping a heartbeat gap: {with_hb}/{len(frames)}; peak dirty+writeback {peak_dirty} MB; max procs_blocked {blocked}")
