# Copyright 2026 Ji Mao Cai
# SPDX-License-Identifier: GPL-2.0-only
"""Verify extracted QEMU Windows I/O functions with MSVC and real scratch files.

No emulator, Android checkout, network, or game data is used. The candidate is
read from --repo; the baseline is read from the requested Git revision.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import statistics
import subprocess


HERE = Path(__file__).resolve().parent
SOURCE = "block/file-win32.c"


def extract(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        depth += (source[pos] == "{") - (source[pos] == "}")
        if not depth:
            return source[start:pos + 1]
    raise ValueError(f"Unterminated function: {signature}")


def functions(source, prefix):
    signatures = ["static ssize_t handle_aiocb_rw_bounce(",
                  "static ssize_t handle_aiocb_rw(", "static int aio_worker("]
    if "static bool raw_win32_iov_is_contiguous(" in source:
        signatures.insert(0, "static bool raw_win32_iov_is_contiguous(")
    result = "\n\n".join(extract(source, s) for s in signatures)
    for name in ["raw_win32_iov_is_contiguous", "handle_aiocb_rw_bounce",
                 "handle_aiocb_rw", "aio_worker"]:
        result = result.replace(name, prefix + name)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--vcvars", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--benchmark", action="store_true")
    parser.add_argument("--rounds", type=int, default=6)
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("This test requires Windows and its real file APIs")
    if args.rounds < 2:
        parser.error("--rounds must be at least 2")
    repo, work = args.repo.resolve(), args.work.resolve()
    if work.exists():
        parser.error("--work must be a fresh directory")
    work.mkdir(parents=True)
    baseline = subprocess.check_output(
        ["git", "show", f"{args.baseline}:{SOURCE}"], cwd=repo).decode("utf-8")
    candidate = (repo / SOURCE).read_text(encoding="utf-8")
    if candidate == baseline:
        parser.error("Candidate equals baseline; no change would be tested")
    if "raw_win32_iov_is_contiguous" not in candidate:
        parser.error("Expected contiguous-vector candidate is absent")
    (work / "extracted.h").write_text(
        functions(baseline, "baseline_") + "\n\n" + functions(candidate, "candidate_"),
        encoding="utf-8")
    (work / "harness.c").write_bytes((HERE / "harness.c").read_bytes())
    # Use cmd only to enter the installed compiler environment. Never delete or
    # move files through another shell. Reject metacharacters in the only path
    # placed in command text; all build outputs are relative to the work dir.
    vcvars = str(args.vcvars.resolve())
    if any(c in vcvars for c in '\"%\r\n!&|<>^'):
        parser.error("Unsupported command metacharacter in --vcvars path")
    script = (f'@echo off\r\ncall "{vcvars}" >nul\r\n'
              'if errorlevel 1 exit /b %errorlevel%\r\n'
              'cl /nologo /TC /std:c11 /O2 /W3 /D_CRT_SECURE_NO_WARNINGS '
              '/wd4244 /wd4267 harness.c /Fe:harness.exe\r\n')
    (work / "build.cmd").write_text(script, encoding="utf-8")
    build = subprocess.run(["cmd.exe", "/d", "/c", "build.cmd"], cwd=work,
                           capture_output=True, text=True)
    (work / "build.log").write_text(build.stdout + build.stderr, encoding="utf-8")
    if build.returncode:
        print(build.stdout, build.stderr)
        raise SystemExit(build.returncode)
    exe = work / "harness.exe"
    checks = subprocess.run([str(exe), "functional", str(work)], capture_output=True, text=True)
    print(checks.stdout, end="")
    (work / "checks.txt").write_text(checks.stdout + checks.stderr, encoding="utf-8")
    result = {
        "baseline_revision": subprocess.check_output(
            ["git", "rev-parse", args.baseline], cwd=repo, text=True).strip(),
        "baseline_sha256": hashlib.sha256(baseline.encode()).hexdigest(),
        "candidate_sha256": hashlib.sha256(candidate.encode()).hexdigest(),
        "functional_exit_code": checks.returncode,
        "scope": "Extracted actual functions; real Windows file I/O; no QEMU process, guest, game, or power-loss test",
    }
    if checks.returncode == 0 and args.benchmark:
        bench = subprocess.run([str(exe), "benchmark", str(work), str(args.rounds)],
                               capture_output=True, text=True)
        (work / "benchmark.csv").write_text(bench.stdout, encoding="utf-8")
        if bench.returncode:
            print(bench.stdout, bench.stderr)
            raise SystemExit(bench.returncode)
        rows = [line.split(",") for line in bench.stdout.splitlines()[1:]]
        groups = {}
        for row in rows:
            name, variant, _, _, seconds, _, _ = row
            groups.setdefault(name, {}).setdefault(variant, []).append(float(seconds))
        result["benchmark_medians"] = {
            name: {"baseline_seconds": statistics.median(v["baseline"]),
                   "candidate_seconds": statistics.median(v["candidate"]),
                   "candidate_vs_baseline_percent":
                       (statistics.median(v["candidate"]) / statistics.median(v["baseline"]) - 1) * 100}
            for name, v in groups.items()
        }
        print(json.dumps(result["benchmark_medians"], indent=2))
    (work / "results.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    raise SystemExit(checks.returncode)


if __name__ == "__main__":
    main()
