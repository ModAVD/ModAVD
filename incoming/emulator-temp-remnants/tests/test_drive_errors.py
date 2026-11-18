# Copyright 2026 Ji Mao Cai
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the actual Win32 allocation-failure guard and share-mode caller.

Run in an x64 VS developer shell:
    python test_drive_errors.py <patched-qemu-tree> [--check-mutation]

This is a source-fragment test, not a QEMU build or a live snapshot test.
The unchanged block/QemuOpts machinery is stubbed. The actual changed guard
is given the empty path returned by a failed temporary allocation, then the
actual updateDriveShareMode caller runs. Its error_get_pretty call must
receive an Error: QEMU's real implementation dereferences err->msg.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def extract_function(text, signature):
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


PREFIX = r'''
#include <string>
#include <cstdio>
namespace android::base { enum class FileShare {Read, Write}; }
struct Error { const char* msg; };
struct QemuOpts {};
struct DriveInitParam {
    int blockDefaultType;
    const char* snapshotName;
    android::base::FileShare shareMode;
};
struct DriveShare { int blockDefaultType; } state{};
DriveShare* sDriveShare = &state;
bool missingError = false;
#define D(...) ((void)0)
bool needCreateTmp(const char*, android::base::FileShare, QemuOpts*) {
    return true;
}
void error_setg(Error** errp, const char*, const char*) {
    static Error error{"temporary allocation failed"};
    if (errp) *errp = &error;
}
const char* error_get_pretty(Error* err) {
    // Observe the precondition without intentionally crashing the test child.
    if (!err) {
        missingError = true;
        return "NULL_ERROR_REACHES_ERROR_GET_PRETTY";
    }
    return err->msg;
}
void error_report(const char*, const char* text) { puts(text); }
void error_free(Error*) {}
void* qemu_find_opts(const char*) { return nullptr; }
int qemu_opts_foreach(void*, int (*callback)(void*, QemuOpts*, Error**),
                      void* opaque, Error** errp) {
    QemuOpts opts;
    return callback(opaque, &opts, errp);
}
'''

MAIN = r'''
int main() {
    bool result = updateDriveShareMode("snapshot", android::base::FileShare::Read);
    printf("result=%d missing_error=%d\n", result, missingError);
    return result ? 2 : missingError ? 77 : 0;
}
'''


def run_case(build, guard, caller, name):
    callback = r'''
static int drive_reinit(void* opaque, QemuOpts* opts, Error** errp) {
    auto* param = static_cast<DriveInitParam*>(opaque);
    const char* id = "cache";
    const std::string path;
    (void)errp;
''' + guard + "\nreturn 0;\n}\n"
    cpp = build / (name + ".cpp")
    cpp.write_text(PREFIX + callback + caller + MAIN, encoding="utf-8")
    exe = build / (name + ".exe")
    subprocess.run(["cl", "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX",
                    str(cpp), "/Fe:" + str(exe)], cwd=build, check=True)
    result = subprocess.run([exe], cwd=build, capture_output=True, text=True, timeout=15)
    print(name + ": " + result.stdout.strip())
    return result.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("qemu", type=Path)
    parser.add_argument("--check-mutation", action="store_true")
    args = parser.parse_args()
    text = (args.qemu / "android-qemu2-glue/drive-share.cpp").read_text(encoding="utf-8")
    reinit = extract_function(text, "static int drive_reinit(")
    guard_start = reinit.index("#ifdef _WIN32", reinit.index("std::string path = initDrivePath"))
    guard_end = reinit.index("#endif", guard_start) + len("#endif")
    guard = reinit[guard_start:guard_end]
    caller = extract_function(text, "static bool updateDriveShareMode(")
    with tempfile.TemporaryDirectory(prefix="modavd-drive-errors-") as directory:
        build = Path(directory)
        result = run_case(build, guard, caller, "candidate")
        if result != 0:
            raise AssertionError("Failed temporary allocation did not propagate an Error: " + str(result))
        if args.check_mutation:
            mutated, count = re.subn(r"error_setg\(errp,[^;]+;", "", guard)
            if count != 1:
                raise AssertionError("Expected one error_setg in the Win32 allocation-failure guard")
            if run_case(build, mutated, caller, "missing-error-mutation") != 77:
                raise AssertionError("Missing-error mutation was not detected")
            print("Missing-error mutation detected.")
    print("Allocation-failure callback contract passed.")


if __name__ == "__main__":
    main()
