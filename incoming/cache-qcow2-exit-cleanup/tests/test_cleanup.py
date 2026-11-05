#!/usr/bin/env python3
# Copyright 2026 Ji Mao Cai
# SPDX-License-Identifier: GPL-2.0-only
"""Compile real tempfile.c and cache setup slices; no emulator or downloads.

Run with Python 3.9+, a native C/C++17 compiler, and --qemu pointing at the
patched mas38 source. On Windows use an MSVC developer shell. On Linux/macOS,
cc/c++ suffice. Only QEMU source and the native compiler/SDK are required.

The adapters replace external path formatting/logging and ext4/drive creation;
file deletion and the process atexit callback are real. This does not boot QEMU
or test read-only QEMU overlays, signals, forced termination, or image contents.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def between(text, first, last):
    assert text.count(first) == 1, first
    start = text.index(first) + len(first)
    end = text.index(last, start)
    return text[start:end]


def run(*args, **kwargs):
    subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, required=True)
    args = parser.parse_args()
    qemu = args.qemu.resolve()
    tests = Path(__file__).resolve().parent
    base = qemu / "android/android-emu-base"
    glue = (qemu / "android-qemu2-glue/main.cpp").read_text(encoding="utf-8")
    common = (qemu / "android/android-emu/android/main-common.c").read_text(encoding="utf-8")
    setup = between(common, "/** CACHE PARTITION **/", "    if (hw->disk_cachePartition_path && opts->cache_size)")
    cache = between(glue, "    bool createEmptyCacheFile = false;", "    android_report_session_phase(ANDROID_SESSION_PHASE_INITACCEL);")
    drive = between(glue, "            case IMAGE_TYPE_CACHE:", "            case IMAGE_TYPE_USER_DATA:")
    assert drive.rstrip().endswith("break;")
    drive = drive.rstrip()[:-len("break;")]
    with tempfile.TemporaryDirectory(prefix="cache-cleanup-") as scratch:
        work = Path(scratch)
        adapters = work / "adapters"
        (adapters / "android/utils").mkdir(parents=True)
        # These external declarations avoid linking the whole aemu logger and
        # compatibility library. The module and native file APIs are unchanged.
        (adapters / "android/utils/debug.h").write_text(
            "void dwarning(const char* fmt, ...);\n", encoding="utf-8")
        (adapters / "android/utils/file_io.h").write_text(
            "int android_unlink(const char* path);\n", encoding="utf-8")
        (adapters / "msvc-posix.h").write_text(
            "#include <windows.h>\n#include <io.h>\n#define close _close\n",
            encoding="utf-8")
        (work / "cache_flow.inc").write_text(
            "static int setupCache(Hardware* hw, Options* opts) {\n"
            "    void* avd = nullptr;\n" + setup +
            "    bool createEmptyCacheFile = false;\n" + cache + "    return 0;\n}\n"
            "static std::string cacheDrivePath(Hardware* m_hw) {\n"
            "    std::string_view filePath;\n"
            "    std::string bufferString, driveParam, deviceParam;\n"
            "    int m_driveIndex = 0;\n"
            "    struct { const char* storageDeviceType; } kTarget = {\"virtio-blk\"};\n" +
            drive + "    return driveParam.substr(driveParam.find(\"file=\") + 5);\n}\n",
            encoding="utf-8")
        sources = [base / "android/utils/tempfile.c", tests / "support.c", tests / "check.cpp"]
        if os.name == "nt":
            run("cl", "/nologo", "/W3", "/std:c11", "/D_CRT_SECURE_NO_WARNINGS",
                "/I" + str(adapters), "/I" + str(base),
                "/c", *sources[:2], cwd=work)
            run("cl", "/nologo", "/W3", "/EHsc", "/std:c++17", "/D_CRT_SECURE_NO_WARNINGS",
                "/I" + str(base), "/I" + str(work), sources[2],
                "tempfile.obj", "support.obj", "/Fe:check.exe", cwd=work)
            exe = work / "check.exe"
        else:
            run("cc", "-std=gnu11", "-I" + str(adapters), "-I" + str(base),
                "-c", *sources[:2], cwd=work)
            run("c++", "-std=c++17", "-I" + str(base), "-I" + str(work),
                sources[2], "tempfile.o", "support.o", "-o", "check", cwd=work)
            exe = work / "check"
        scenarios = ["register", "existing", "close", "missing", "duplicate", "raw", "extension",
                     "disabled", "disabled-empty", "no-cache", "default", "default-missing", "read-only"]
        for scenario in scenarios:
            for repeat in range(3):
                lab = work / f"{scenario}-{repeat}"
                lab.mkdir()
                env = dict(os.environ, TEMPFILE_TEST_DIR=str(lab),
                           PERMANENT_CACHE=str(lab / "cache.img"))
                run(exe, scenario, lab, env=env)
                names = {path.name for path in lab.iterdir()}
                expected = {"unrelated"} if scenario == "existing" else set()
                if scenario in ("default", "default-missing", "read-only"):
                    expected = {"cache.img", "cache.img.qcow2"}
                assert names == expected, (scenario, repeat, names, expected)
            print(f"PASS {scenario}: 3 normal child exits", flush=True)
        print(f"PASS {len(scenarios) * 3} child exits: original paths, no random sidecar, cleanup, cache controls")


if __name__ == "__main__":
    main()
