#!/usr/bin/env python3
"""Build a pinned, clean ModAVDManager checkout into a new macOS ARM64 bundle."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
UPSTREAM = "https://github.com/ModAVD/ModAVDManager.git"
TOOLCHAIN = "go1.26.5"
RESERVE = 20 * 1024**3
RECEIPT = ".modavd-manager-build.json"


def run(argv, *, cwd=None, env=None, capture=False, timeout=1200):
    print("+", " ".join(map(str, argv)), flush=True)
    return subprocess.run(list(map(str, argv)), cwd=cwd, env=env, check=True,
                          stdout=subprocess.PIPE if capture else None,
                          stderr=subprocess.PIPE if capture else None, text=True, timeout=timeout)


def space(path):
    while not path.exists():
        path = path.parent
    if shutil.disk_usage(path).free < RESERVE:
        raise RuntimeError("Build stopped: at least 20 GiB must remain free")


def digest(path):
    with path.open("rb") as incoming:
        return hashlib.file_digest(incoming, "sha256").hexdigest()


def version_fields(version, build):
    if not isinstance(version, str) or type(build) is not int:
        raise RuntimeError("Package version and build must be a string and integer")
    match = re.fullmatch(r"v?(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", version)
    if not match or build <= 0 or int(match[2]) != build:
        raise RuntimeError("--version must be X.Y.Z and --build must equal Y (the ModAVD core build)")
    return ".".join(match.groups())


def source_state(source, revision):
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise RuntimeError("--revision must be the full 40-character Manager commit SHA")
    def git(*args):
        return run(["git", "-C", source, *args], capture=True, timeout=30).stdout.strip()
    if git("rev-parse", "--show-toplevel") != str(source):
        raise RuntimeError("--source must be the checkout root")
    if git("rev-parse", "HEAD") != revision:
        raise RuntimeError("Manager HEAD differs from --revision; the checkout was not modified")
    if git("status", "--porcelain", "--untracked-files=all"):
        raise RuntimeError("Manager checkout must be clean; local changes were retained")
    ignored = git("ls-files", "--others", "--ignored", "--exclude-standard").splitlines()
    replaced = {"internal/guest/proxyd/modavd-proxyd.gz", "internal/guest/proxyd/modavd-proxyd-arm64.gz"}
    if any(path not in replaced for path in ignored):
        raise RuntimeError("Ignored build inputs exist outside the two replaced guest payloads; choose a clean checkout")
    if not (source / "go.mod").read_text().startswith("module modavd\n"):
        raise RuntimeError("Not a ModAVDManager module")
    text = (source / "internal/buildinfo/buildinfo.go").read_text()
    match = re.search(r'^var Version = "([0-9]+\.[0-9]+\.[0-9]+)"$', text, re.M)
    protocol = re.search(r"^const Protocol = ([0-9]+)$", text, re.M)
    if not match or not protocol:
        raise RuntimeError("Cannot read Manager buildinfo version/protocol")
    return {"revision": revision, "tree": git("rev-parse", "HEAD^{tree}"),
            "managerVersion": match[1], "protocol": int(protocol[1])}


def prepare(source, revision):
    # Only a newly created clone is checked out. An existing checkout is read-only.
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise RuntimeError("--revision must be the full 40-character Manager commit SHA")
    if not source.exists():
        source.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", "--no-checkout", UPSTREAM, source])
        run(["git", "fetch", "--depth=1", "origin", revision], cwd=source)
        run(["git", "checkout", "--detach", revision], cwd=source)
    return source_state(source, revision)


def guest_payload(source, temporary, env):
    replacement = {}
    receipts = {}
    for arch, abi, machine, filename in (("amd64", "x86_64", 62, "modavd-proxyd.gz"),
                                         ("arm64", "arm64-v8a", 183, "modavd-proxyd-arm64.gz")):
        target = temporary / ("proxyd-" + arch)
        guest_env = {**env, "GOOS": "linux", "GOARCH": arch, "CGO_ENABLED": "0"}
        run(["go", "build", "-trimpath", "-buildvcs=false", "-ldflags=-s -w", "-o", target,
             "./cmd/modavd-proxyd"], cwd=source, env=guest_env)
        payload = target.read_bytes()
        if (len(payload) > 64 << 20 or payload[:6] != b"\x7fELF\x02\x01" or
                len(payload) < 20 or struct.unpack_from("<H", payload, 18)[0] != machine or
                struct.unpack_from("<H", payload, 16)[0] not in (2, 3)):
            raise RuntimeError("Built guest daemon has the wrong ELF ABI: " + abi)
        compressed = temporary / filename
        # GzipFile fixes both filename and timestamp, independent of the temp path.
        with compressed.open("wb") as output:
            with gzip.GzipFile(filename="", mode="wb", fileobj=output, mtime=0) as archive:
                archive.write(payload)
        replacement[str(source / "internal/guest/proxyd" / filename)] = str(compressed)
        receipts[abi] = {"machine": machine, "bytes": len(payload),
                         "sha256": hashlib.sha256(payload).hexdigest(), "gzipSha256": digest(compressed)}
    overlay = temporary / "overlay.json"
    overlay.write_text(json.dumps({"Replace": replacement}) + "\n")
    return overlay, receipts


def build(args):
    if sys.platform != "darwin" or platform.machine() != "arm64":
        raise RuntimeError("Build requires an Apple Silicon Mac and the macOS SDK")
    version = version_fields(args.version, args.build)
    if args.notary_profile and args.sign_identity == "-":
        raise RuntimeError("Notarization requires an explicit Developer ID Application signing identity")
    signing = ["/usr/bin/codesign", "--force", "--sign", args.sign_identity]
    if args.sign_identity != "-":
        signing += ["--options", "runtime", "--timestamp"]
    source = args.source
    space(source.parent)
    state = prepare(source, args.revision)
    output = args.output
    if os.path.lexists(output):
        raise RuntimeError("Output already exists; choose a new --output")
    if output.is_relative_to(source) or source.is_relative_to(output):
        raise RuntimeError("Source and output must not overlap")
    output.parent.mkdir(parents=True, exist_ok=True)
    space(output.parent)
    env = os.environ.copy()
    env.update(GOTOOLCHAIN=TOOLCHAIN, GOOS="darwin", GOARCH="arm64", CGO_ENABLED="1",
               GOFLAGS="", GOENV="off", GOWORK="off", MACOSX_DEPLOYMENT_TARGET="13.0")
    if args.developer_dir is not None:
        env["DEVELOPER_DIR"] = str(args.developer_dir)
    # Everything is generated outside the source checkout, including go:embed inputs.
    with tempfile.TemporaryDirectory(prefix=".modavd-manager-", dir=output.parent) as directory:
        temporary = Path(directory)
        stage = temporary / "dist"
        tools = stage / "tools"
        tools.mkdir(parents=True)
        overlay, payloads = guest_payload(source, temporary, env)
        if not args.skip_checks:
            run(["go", "test", "-overlay", overlay, "./..."], cwd=source, env=env)
            run(["go", "vet", "-overlay", overlay, "./..."], cwd=source, env=env)
        built_hashes = {}
        embedded = [Path(path).read_bytes() for path in json.loads(overlay.read_text())["Replace"].values()]
        for name, package in (("modavd", "."), ("modavd-manager", "./cmd/modavd-gui")):
            space(output.parent)
            run(["go", "build", "-overlay", overlay, "-trimpath", "-buildvcs=false", "-ldflags=-s -w",
                 "-o", tools / name, package], cwd=source, env=env)
            binary = (tools / name).read_bytes()
            if any(payload not in binary for payload in embedded):
                raise RuntimeError("Built host executable lacks a generated guest payload: " + name)
            built_hashes[name] = hashlib.sha256(binary).hexdigest()
            run([*signing, tools / name], env=env, timeout=120)
        contents = stage / "ModAVD.app/Contents"
        binaries = contents / "MacOS"
        binaries.mkdir(parents=True)
        shutil.copy2(tools / "modavd-manager", binaries / "ModAVD")
        info = {"CFBundleName": "ModAVD", "CFBundleDisplayName": "ModAVD", "CFBundleExecutable": "ModAVD",
                "CFBundleIdentifier": "org.modavd.manager", "CFBundlePackageType": "APPL",
                "CFBundleShortVersionString": state["managerVersion"], "CFBundleVersion": str(args.build),
                "LSMinimumSystemVersion": "13.0", "NSHighResolutionCapable": True}
        (contents / "Info.plist").write_bytes(plistlib.dumps(info))
        run([*signing, stage / "ModAVD.app"], env=env, timeout=120)
        # Bundle signing binds Info.plist; the standalone entry keeps its own signature.
        run(["/usr/bin/codesign", "--verify", "--deep", "--strict", stage / "ModAVD.app"], timeout=60)
        for name in ("modavd", "modavd-manager"):
            run(["/usr/bin/codesign", "--verify", "--strict", tools / name], timeout=60)
        if args.notary_profile:
            submission = temporary / "notarize.zip"
            run(["/usr/bin/ditto", "-c", "-k", "--keepParent", stage / "ModAVD.app", submission], timeout=120)
            result = run(["/usr/bin/xcrun", "notarytool", "submit", submission,
                          "--keychain-profile", args.notary_profile, "--wait", "--output-format", "json"],
                         capture=True, timeout=1200)
            if json.loads(result.stdout).get("status") != "Accepted":
                raise RuntimeError("Apple did not accept the app notarization submission")
            run(["/usr/bin/xcrun", "stapler", "staple", stage / "ModAVD.app"], timeout=120)
            run(["/usr/bin/xcrun", "stapler", "validate", stage / "ModAVD.app"], timeout=120)
            run(["/usr/sbin/spctl", "--assess", "--type", "execute", stage / "ModAVD.app"], timeout=120)
        for name in ("LICENSE", "NOTICE"):
            if (source / name).is_file():
                shutil.copy2(source / name, stage / name)
        if source_state(source, args.revision) != state:
            raise RuntimeError("Manager source changed during the build")
        receipt = {"schema": 2, "upstream": UPSTREAM, **state, "toolchain": TOOLCHAIN,
                   "platform": "macos-arm64", "version": version, "build": args.build,
                   "signature": "ad-hoc-integrity" if args.sign_identity == "-" else "identity-signed",
                   "appNotarized": bool(args.notary_profile), "checks": "skipped" if args.skip_checks else "test-and-vet",
                   "builtBinarySha256": built_hashes, "guestPayloads": payloads}
        (stage / RECEIPT).write_text(json.dumps(receipt, indent=2) + "\n")
        checksums = {p.relative_to(stage).as_posix(): digest(p) for p in sorted(stage.rglob("*")) if p.is_file()}
        (stage / "SHA256SUMS.json").write_text(json.dumps(checksums, indent=2) + "\n")
        space(output.parent)
        # Atomic, exclusive publication. Do not overwrite a path created during the build.
        import ctypes
        libc = ctypes.CDLL(None, use_errno=True)
        rename = libc.renamex_np
        rename.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint)
        rename.restype = ctypes.c_int
        if rename(os.fsencode(stage), os.fsencode(output), 4):
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error), str(output))
    print(json.dumps({"output": str(output), **receipt}, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("prepare", "build"), nargs="?", default="build")
    parser.add_argument("--source", type=Path, default=ROOT / ".macos-work/manager")
    parser.add_argument("--revision", required=True, help="Exact clean ModAVDManager commit; never a moving branch")
    parser.add_argument("--version", help="ModAVD package version X.Y.Z (Manager version comes from its buildinfo)")
    parser.add_argument("--build", type=int, help="ModAVD core build Y from X.Y.Z")
    parser.add_argument("--output", type=Path, default=ROOT / ".macos-work/manager-dist")
    parser.add_argument("--developer-dir", type=Path)
    parser.add_argument("--sign-identity", default="-", help="Explicit local codesign identity; default is ad hoc")
    parser.add_argument("--notary-profile", help="Explicit notarytool keychain profile; submits only when provided")
    parser.add_argument("--skip-checks", action="store_true", help="Build already verified source; recorded in receipt")
    args = parser.parse_args()
    args.source = args.source.expanduser().resolve()
    args.output = Path(os.path.abspath(args.output.expanduser()))
    args.output = args.output.parent.resolve() / args.output.name
    if args.action == "prepare":
        prepare(args.source, args.revision)
    else:
        if args.version is None or args.build is None:
            parser.error("build requires --version and --build")
        build(args)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError, subprocess.SubprocessError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)
