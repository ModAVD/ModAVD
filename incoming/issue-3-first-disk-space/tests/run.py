# Copyright 2026 Ji Mao Cai
# SPDX-License-Identifier: GPL-2.0-only
"""Compile and exercise the first-creation check extracted from main.cpp."""

import argparse
from pathlib import Path
import shutil
import subprocess


def sections(source):
    newline = "\r\n" if "\r\n" in source else "\n"
    start = source.index(newline + "#ifdef _WIN32" + newline, source.index("static void replaceDefaultPath"))
    end = source.index(newline + "// MAS local: grow (never shrink)", start)
    helpers = source[start:end]
    check_start = source.index("        // Check free space first", source.index("// Create userdata file"))
    check_end = source.index("        // pixel_fold quirk", check_start)
    return helpers, source[check_start:check_end], source[:start] + source[end:check_start] + source[check_end:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True, help="QEMU source root")
    parser.add_argument("--work", type=Path, required=True, help="Fresh output directory")
    parser.add_argument("--cxx", default="cl" if shutil.which("cl") else "c++")
    parser.add_argument("--baseline", help="Also verify all source outside the two edited sections is unchanged")
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=False)
    source = (args.source / "android-qemu2-glue/main.cpp").read_bytes().decode("utf-8")
    helpers, check, rest = sections(source)
    if args.baseline:
        original = subprocess.check_output(
            ["git", "-C", str(args.source), "show", args.baseline + ":android-qemu2-glue/main.cpp"]
        ).decode("utf-8")
        if sections(original)[2] != rest:
            raise SystemExit("Source changed outside volume identification and the first-creation check")
        print("Creation, conversion, resizing and all other main.cpp code unchanged", flush=True)
    template = Path(__file__).with_name("harness.cpp.in").read_text(encoding="utf-8")
    harness = args.work / "first-disk-space.cpp"
    harness.write_text(template.replace("@HELPERS@", helpers).replace("@CHECK@", check), encoding="utf-8")
    msvc = Path(args.cxx).stem.lower() == "cl"
    failures = []
    for name, defines in (("windows", []), ("posix", ["TEST_POSIX"])):
        output = args.work / (name + (".exe" if msvc else ""))
        if msvc:
            command = [args.cxx, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX", "/wd4505",
                       *["/D" + item for item in defines], str(harness),
                       "/Fe:" + str(output), "/Fo:" + str(args.work / (name + ".obj"))]
        else:
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                       *["-D" + item for item in defines], str(harness), "-o", str(output)]
        subprocess.run(command, check=True, cwd=args.work)
        if subprocess.run([str(output)], cwd=args.work).returncode:
            failures.append(name)
    if failures:
        raise SystemExit("FAILED: " + ", ".join(failures))


if __name__ == "__main__":
    main()
