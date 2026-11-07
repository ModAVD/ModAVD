#!/usr/bin/env python3
"""Assemble a new macOS ARM64 package from checked local inputs; never update an installation."""

from __future__ import annotations

import argparse
import base64
import ctypes
import datetime as dt
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import plistlib
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import time
import types
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[2]
RESERVE = 20 * 1024**3
MARKER = ".modavd-macos-package.json"
MANIFEST = "PACKAGE-MANIFEST.json"
PLATFORM = "macos-arm64"
IMAGE = "system-images/android-35/google_apis/arm64-v8a"
PAYLOAD_DIRS = ("sdk", "tools", "ModAVD.app", "components", "provenance", "licenses")
PAYLOAD_FILES = (MARKER, "README.zh-CN.txt", "modavd-manifest.json")
SIGNATURE_XATTRS = frozenset(("com.apple.cs.CodeDirectory", "com.apple.cs.CodeRequirements",
                             "com.apple.cs.CodeSignature", "com.apple.cs.CodeEntitlements"))
APK_LOCK_PATH = ROOT / "ports/macos/components.lock.json"
APK_LOCK = json.loads(APK_LOCK_PATH.read_text())
IME, LAUNCHER = (APK_LOCK["components"][name] for name in ("ime", "launcher"))
LIBC = ctypes.CDLL(None, use_errno=True)
if sys.platform == "darwin":
    for name, arguments in (
        ("listxattr", (ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int)),
        ("getxattr", (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_int)),
        ("setxattr", (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_int)),
        ("removexattr", (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int)),
    ):
        function = getattr(LIBC, name)
        function.argtypes = arguments
        function.restype = ctypes.c_ssize_t if name in ("listxattr", "getxattr") else ctypes.c_int


def fail(message):
    raise RuntimeError(message)


def xattr_result(result, path):
    if result < 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), str(path))
    return result


def list_xattrs(path):
    if sys.platform != "darwin":
        return os.listxattr(path, follow_symlinks=False)
    size = xattr_result(LIBC.listxattr(os.fsencode(path), None, 0, 1), path)
    if size > 65536:
        fail("Extended attribute names exceed 64 KiB: " + str(path))
    if not size:
        return []
    buffer = ctypes.create_string_buffer(size)
    length = xattr_result(LIBC.listxattr(os.fsencode(path), buffer, size, 1), path)
    return [os.fsdecode(name) for name in buffer.raw[:length].split(b"\0") if name]


def get_xattr(path, name):
    if sys.platform != "darwin":
        return os.getxattr(path, name, follow_symlinks=False)
    args = (os.fsencode(path), os.fsencode(name))
    size = xattr_result(LIBC.getxattr(*args, None, 0, 0, 1), path)
    if size > 1 << 20:
        fail("Code signature attribute exceeds 1 MiB: " + str(path))
    buffer = ctypes.create_string_buffer(size)
    length = xattr_result(LIBC.getxattr(*args, buffer, size, 0, 1), path)
    if length != size:
        fail("Extended attribute changed while reading: " + str(path))
    return buffer.raw[:length]


def set_xattr(path, name, value):
    if sys.platform != "darwin":
        return os.setxattr(path, name, value, follow_symlinks=False)
    buffer = ctypes.create_string_buffer(value)
    xattr_result(LIBC.setxattr(os.fsencode(path), os.fsencode(name), buffer, len(value), 0, 1), path)


def remove_xattr(path, name):
    if sys.platform != "darwin":
        return os.removexattr(path, name, follow_symlinks=False)
    xattr_result(LIBC.removexattr(os.fsencode(path), os.fsencode(name), 1), path)


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def contains(path, needle):
    tail = b""
    with path.open("rb") as source:
        while chunk := source.read(1 << 20):
            if needle in tail + chunk:
                return True
            tail = chunk[-len(needle):]
    return False


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")


def space(path, additional=0):
    while not path.exists():
        path = path.parent
    free = shutil.disk_usage(path).free
    if free - additional < RESERVE:
        fail(f"Space stopped: {free / 1024**3:.1f} GiB free, {additional / 1024**3:.1f} GiB next; retain 20 GiB")
    return free


def relative_name(name):
    value = PurePosixPath(name)
    if (not name or value.as_posix() != name or value.is_absolute() or ".." in value.parts or
            "\\" in name or name == "." or any(ord(c) < 32 or ord(c) > 126 for c in name)):
        fail(f"Unsafe/non-ASCII payload path: {name!r}")
    return Path(*value.parts)


def entries(root, boundary=None):
    """Inspect without following directory links; a link must stay inside its unit."""
    boundary = (boundary or root).resolve()
    for parent, directories, files in os.walk(root, followlinks=False,
                                            onerror=lambda error: fail(f"Cannot read input: {error}")):
        directories.sort()
        for name in sorted(directories + files):
            path = Path(parent) / name
            relative_name(path.relative_to(root).as_posix())
            mode = path.lstat().st_mode
            if stat.S_ISLNK(mode):
                target = os.readlink(path)
                if (not target or len(target) > 4096 or os.path.isabs(target) or "\\" in target or
                        any(ord(c) < 32 or ord(c) > 126 for c in target) or
                        not path.resolve(strict=True).is_relative_to(boundary)):
                    fail(f"External/absolute link rejected: {path} -> {target}")
                # Each intermediate '..' must stay within the unit too.
                current = path.parent
                for part in PurePosixPath(target).parts:
                    current = current.parent if part == ".." else current / part
                    if not current.resolve(strict=True).is_relative_to(boundary):
                        fail(f"Link escapes its unit: {path}")
            elif not (stat.S_ISDIR(mode) or stat.S_ISREG(mode)) or stat.S_IMODE(mode) & ~0o777:
                fail(f"Special file/permissions rejected: {path}")
            yield path


def signature_xattrs(path):
    if sys.platform != "darwin":
        return {}
    names = list_xattrs(path)
    if any(n.startswith("com.apple.cs.") and n not in SIGNATURE_XATTRS for n in names):
        fail("Unsupported code signature attributes: " + str(path))
    result = {}
    for name in sorted(set(names) & SIGNATURE_XATTRS):
        value = get_xattr(path, name)
        if len(value) > 1 << 20:
            fail("Code signature attribute exceeds 1 MiB")
        result[name] = base64.b64encode(value).decode("ascii")
    return result


def arm64_executable(path):
    if path.is_symlink() or not path.is_file() or not path.stat().st_mode & 0o111 or not macho(path):
        fail(f"Missing real ARM64 executable: {path}")


def macho(path):
    with path.open("rb") as source:
        header = source.read(4096)
    architectures = []
    if header[:4] in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf") and len(header) >= 8:
        endian = "<" if header[:4] == b"\xcf\xfa\xed\xfe" else ">"
        architectures = [struct.unpack_from(endian + "I", header, 4)[0]]
    elif header[:4] in (b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf", b"\xbe\xba\xfe\xca", b"\xbf\xba\xfe\xca") and len(header) >= 8:
        endian = ">" if header[0] == 0xCA else "<"
        stride = 32 if header[:4] in (b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca") else 20
        count = struct.unpack_from(endian + "I", header, 4)[0]
        if count > 64 or 8 + stride * count > len(header):
            fail(f"Invalid fat Mach-O header: {path}")
        architectures = [struct.unpack_from(endian + "I", header, 8 + stride * i)[0] for i in range(count)]
    if architectures and 0x0100000C not in architectures:
        fail(f"Mach-O lacks ARM64 code: {path}")
    return bool(architectures)


def verify_signatures(directory, bundle=False):
    if sys.platform != "darwin":
        fail("Signature verification requires macOS")
    deadline = time.monotonic() + 120
    targets = [p for p in entries(directory) if p.is_file() and not p.is_symlink() and macho(p)]
    if bundle:
        targets.append(directory)
    for target in targets:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            fail("Code signature verification exceeded two minutes")
        result = subprocess.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(target)],
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=min(30, remaining))
        if result.returncode:
            fail(f"Signature integrity failed: {target}: {result.stderr[:8192].decode(errors='replace')}")


def properties(path):
    result = {}
    for line in path.read_text().splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            key, value = (s.strip() for s in line.split("=", 1))
            if key in result:
                fail("Duplicate source.properties key: " + key)
            result[key] = value
    return result


def check_apk(path, config, label):
    if path.is_symlink() or not path.is_file() or path.stat().st_size != config["size"] or digest(path) != config["sha256"]:
        fail(f"{label} APK differs from fixed size/SHA256: {path}")


def check_launcher(path):
    check_apk(path, LAUNCHER, "Launcher")
    for file_key, hash_key in (("licenseFile", "licenseSha256"), ("embeddedLicenseFile", "embeddedLicenseSha256")):
        if digest(ROOT / "ports/macos/licenses" / LAUNCHER[file_key]) != LAUNCHER[hash_key]:
            fail("Launcher license differs from the component lock")
    with zipfile.ZipFile(path) as apk:
        if "classes.dex" not in apk.namelist():
            fail("Launcher APK lacks classes.dex")
        if hashlib.sha256(apk.read(LAUNCHER["embeddedLicenseMember"])).hexdigest() != LAUNCHER["embeddedLicenseSha256"]:
            fail("Launcher embedded license differs from its lock")
        for member in apk.infolist():
            if member.filename.startswith("lib/") or member.filename.lower().endswith((".so", ".dll", ".dylib")):
                fail("Launcher contains unexpected native code")
            if not member.is_dir():
                with apk.open(member) as source:
                    if source.read(4) == b"\x7fELF":
                        fail("Launcher contains unexpected ELF code")


def verify_receipt(directory, files):
    if directory.is_symlink() or not directory.is_dir() or not isinstance(files, dict):
        fail(f"Receipt requires a real component directory and file map: {directory}")
    actual = {p.relative_to(directory).as_posix() for p in entries(directory) if not p.is_dir() or p.is_symlink()}
    if actual != set(files):
        fail(f"Input file set differs from receipt: {directory}")
    for name, expected in files.items():
        path = directory / relative_name(name)
        if isinstance(expected, dict):
            if expected.get("type") == "symlink":
                target = os.readlink(path) if path.is_symlink() else None
                if not isinstance(target, str) or target != expected.get("target") or hashlib.sha256(target.encode()).hexdigest() != expected.get("sha256"):
                    fail(f"Receipt link mismatch: {path}")
                continue
            if expected.get("type") != "file" or expected.get("mode") != stat.S_IMODE(path.lstat().st_mode):
                fail(f"Receipt type/mode mismatch: {path}")
            expected = expected.get("sha256", "")
        if path.is_symlink() or not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected) or digest(path) != expected:
            fail(f"Receipt SHA256 mismatch: {path}")


def manager_module():
    path = ROOT / "ports/macos/manager/build.py"
    module = types.ModuleType("modavd_manager_builder")
    module.__file__ = str(path)
    exec(compile(path.read_bytes(), str(path), "exec"), module.__dict__)
    return module


def validate_inputs(args):
    emulator, manager, runtime = args.emulator, args.manager, args.runtime_root
    for directory in (emulator, manager, runtime):
        if directory.is_symlink() or not directory.is_dir():
            fail(f"Input is not a real directory: {directory}")
    builder = manager_module()
    version = builder.version_fields(args.version, args.build)
    for name in ("emulator", "qemu-img", "qemu/darwin-aarch64/qemu-system-aarch64", "qemu/darwin-aarch64/qemu-system-aarch64-headless"):
        arm64_executable(emulator / name)
    props = properties(emulator / "source.properties")
    if args.profile == "official-validation":
        if props.get("Pkg.BuildId") != "16740729" or props.get("Pkg.Revision") not in ("37.2.13", "37.2.13.0"):
            fail("official-validation requires unchanged official emulator 37.2.13 / build 16740729")
    else:
        if props.get("Pkg.BuildId") != f"mas-{args.build}" or not props.get("Pkg.Revision"):
            fail("Custom core BuildId must already match --build; it is never rewritten")
        with (emulator / "qemu/darwin-aarch64/qemu-system-aarch64-headless").open("rb") as incoming:
            tail = b""
            while data := incoming.read(1 << 20):
                if b"free-page-reporting" in tail + data:
                    break
                tail = data[-32:]
            else:
                fail("Custom core lacks the free-page-reporting capability marker")
        if args.core_receipt is None:
            fail("Custom builds require --core-receipt with source identity and complete file hashes")
    core = {"profile": args.profile, "sourceProperties": props,
            "identity": "local-signed-input" if args.profile == "official-validation" else "custom-build-receipt"}
    if args.core_receipt:
        receipt = json.loads(args.core_receipt.read_text())
        if not isinstance(receipt.get("source"), dict) or not receipt["source"] or not receipt.get("files"):
            fail("Core receipt requires source identity and complete files")
        verify_receipt(emulator, receipt["files"])
        core["receipt"] = receipt
    for name in ("NOTICE.txt", "NOTICE.csv"):
        if not (emulator / name).is_file() or not (emulator / name).stat().st_size:
            fail("Core lacks license inventory: " + name)
    receipt = json.loads((manager / builder.RECEIPT).read_text())
    state = builder.source_state(args.manager_source, args.manager_revision)
    if (receipt.get("schema") != 2 or receipt.get("upstream") != builder.UPSTREAM or
            receipt.get("platform") != PLATFORM or receipt.get("version") != version or
            receipt.get("build") != args.build or any(receipt.get(k) != v for k, v in state.items()) or
            set(receipt.get("guestPayloads", {})) != {"x86_64", "arm64-v8a"}):
        fail("Manager receipt differs from the exact source/platform/package identity")
    binaries = receipt.get("builtBinarySha256", {})
    if (set(binaries) != {"modavd", "modavd-manager"} or
            any(not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value) for value in binaries.values())):
        fail("Manager receipt lacks the built host executable identities")
    for abi, machine in (("x86_64", 62), ("arm64-v8a", 183)):
        payload = receipt["guestPayloads"][abi]
        if (not isinstance(payload, dict) or payload.get("machine") != machine or
                type(payload.get("bytes")) is not int or not 20 <= payload["bytes"] <= 64 << 20 or
                any(not isinstance(payload.get(key), str) or not re.fullmatch(r"[0-9a-f]{64}", payload[key])
                    for key in ("sha256", "gzipSha256"))):
            fail("Manager receipt has an invalid guest payload identity: " + abi)
    checksums = json.loads((manager / "SHA256SUMS.json").read_text())
    verify_receipt(manager, {**checksums, "SHA256SUMS.json": digest(manager / "SHA256SUMS.json")})
    for name in ("tools/modavd", "tools/modavd-manager", "ModAVD.app/Contents/MacOS/ModAVD"):
        arm64_executable(manager / name)
    info = plistlib.loads((manager / "ModAVD.app/Contents/Info.plist").read_bytes())
    if (info.get("CFBundleExecutable") != "ModAVD" or info.get("CFBundleShortVersionString") != state["managerVersion"] or
            info.get("CFBundleVersion") != str(args.build)):
        fail("Manager plist and build receipt disagree")
    lock = json.loads((ROOT / "ports/macos/runtime/components.lock.json").read_text())
    receipts = {}
    for name in ("platform-tools", "system-image"):
        config = lock["components"][name]
        component = json.loads((runtime / "receipts" / (name + ".json")).read_text())
        if (component.get("archiveSha1") != config["sha1"] or component.get("revision") != config["revision"] or
                not re.fullmatch(r"[0-9a-f]{64}", component.get("archiveSha256", "")) or
                config.get("sha256", component["archiveSha256"]) != component["archiveSha256"]):
            fail("SDK receipt differs from fixed archive: " + name)
        accepted = json.loads((runtime / "receipts" / (config["license"] + ".json")).read_text())
        if accepted.get("id") != config["license"] or not accepted.get("acceptedAt") or accepted.get("sha256") != config["licenseTextSha256"]:
            fail("Missing explicit license receipt: " + config["license"])
        license_file = ROOT / "ports/macos/runtime/licenses" / (config["license"] + ".txt")
        if digest(license_file) != config["licenseTextSha256"]:
            fail("License text differs from fixed archive metadata")
        destination = runtime / "sdk" / config["destination"]
        verify_receipt(destination, component.get("files", {}))
        values = properties(destination / "source.properties")
        if values.get("Pkg.Revision") != config["revision"] or any(values.get(k) != v for k, v in config.get("requiredProperties", {}).items()):
            fail("SDK source.properties differs from the ARM64/API35 profile")
        receipts[name] = component
    arm64_executable(runtime / "sdk/platform-tools/adb")
    for name in ("system.img", "vendor.img", "kernel-ranchu", "ramdisk.img"):
        if not (runtime / "sdk" / IMAGE / name).is_file():
            fail("API35 image lacks " + name)
    if args.ime_apk:
        check_apk(args.ime_apk, IME, "IME")
    if args.launcher_apk:
        check_launcher(args.launcher_apk)
    verify_signatures(emulator, bundle=args.profile == "custom")
    verify_signatures(manager)
    verify_signatures(manager / "ModAVD.app", bundle=True)
    verify_signatures(runtime / "sdk/platform-tools")
    return version, core, receipt, lock, receipts


def copy_file(source, destination, stats):
    before = source.stat()
    space(destination.parent)
    attributes = signature_xattrs(source)
    cloned = False
    if sys.platform == "darwin":
        clonefile = LIBC.clonefile
        clonefile.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int)
        clonefile.restype = ctypes.c_int
        cloned = clonefile(os.fsencode(source), os.fsencode(destination), 0) == 0
    if cloned:
        stats["clonedFiles"] += 1
    else:
        if os.path.lexists(destination):
            fail(f"Copy destination already exists: {destination}")
        space(destination.parent, before.st_size)
        with source.open("rb") as src, destination.open("xb") as dst:
            while chunk := src.read(1 << 20):
                space(destination.parent, len(chunk))
                dst.write(chunk)
        stats["copiedFiles"] += 1
    shutil.copystat(source, destination)
    if sys.platform == "darwin":
        for name in list_xattrs(destination):
            if name not in SIGNATURE_XATTRS:
                remove_xattr(destination, name)
        for name, encoded in attributes.items():
            set_xattr(destination, name, base64.b64decode(encoded))
    after = source.stat()
    if ((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) !=
            (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns) or
            digest(source) != digest(destination) or signature_xattrs(source) != attributes or
            signature_xattrs(destination) != attributes):
        fail(f"Input changed while copying: {source}")
    space(destination.parent)


def copy_tree(source, destination, stats):
    destination.mkdir(parents=True)
    directories = [source]
    for path in entries(source):
        target = destination / path.relative_to(source)
        if path.is_symlink():
            target.symlink_to(os.readlink(path))
        elif path.is_dir():
            target.mkdir()
            directories.append(path)
        else:
            copy_file(path, target, stats)
    for path in reversed(directories):
        target = destination / path.relative_to(source)
        shutil.copystat(path, target)
        # Generic (flat bundle) signatures may live on the directory itself.
        for name, value in signature_xattrs(path).items():
            set_xattr(target, name, base64.b64decode(value))


def inventory(root, immutable_only=False):
    paths = []
    if immutable_only:
        for name in PAYLOAD_FILES:
            path = root / name
            if path.is_symlink() or not path.is_file():
                fail("Missing package metadata: " + name)
            paths.append(path)
        for name in PAYLOAD_DIRS:
            path = root / name
            if path.is_symlink() or not path.is_dir():
                fail("Missing package directory: " + name)
            paths.extend([path, *entries(path)])
    else:
        paths = entries(root)
    result, folded = {}, {}
    for path in paths:
        name = path.relative_to(root).as_posix()
        if name == MANIFEST:
            continue
        relative_name(name)
        if name.lower() in folded and folded[name.lower()] != name:
            fail("Case-ambiguous package paths: " + name)
        folded[name.lower()] = name
        info = path.lstat()
        record = {"mode": stat.S_IMODE(info.st_mode)}
        if path.is_symlink():
            target = os.readlink(path)
            record.update(type="symlink", target=target, sha256=hashlib.sha256(target.encode()).hexdigest())
        elif path.is_dir():
            record.update(type="directory")
        else:
            record.update(type="file", bytes=info.st_size, sha256=digest(path))
        attributes = signature_xattrs(path)
        if attributes:
            record["xattrs"] = attributes
        result[name] = record
    return result


def verify(output):
    if output.is_symlink() or (output / MARKER).is_symlink() or not (output / MARKER).is_file():
        fail("Output lacks an owned package marker")
    if (output / MANIFEST).is_symlink() or not (output / MANIFEST).is_file():
        fail("Output lacks its immutable inventory")
    marker = json.loads((output / MARKER).read_text())
    manifest = json.loads((output / MANIFEST).read_text())
    if (marker.get("schema") != 1 or marker.get("tool") != "ModAVD macOS local assembler" or
            manifest.get("schema") != 2 or manifest.get("owner") != marker.get("owner") or
            manifest.get("platform") != PLATFORM or manifest.get("profile") not in ("custom", "official-validation")):
        fail("Package marker and inventory disagree")
    version = manager_module().version_fields(manifest.get("version", ""), manifest.get("build", 0))
    identity = json.loads((output / "modavd-manifest.json").read_text())
    if any(identity.get(key) != manifest.get(key) for key in ("platform", "version", "build", "channel", "profile")):
        fail("Package manifest and runtime identity disagree")
    if version != manifest["version"]:
        fail("Package version must use its normalized X.Y.Z form")
    if inventory(output, immutable_only=True) != manifest.get("files"):
        fail("Package payload differs from its inventory")
    verify_signatures(output / "sdk/emulator", bundle=manifest.get("profile") == "custom")
    verify_signatures(output / "ModAVD.app", bundle=True)
    verify_signatures(output / "tools")
    verify_signatures(output / "sdk/platform-tools")
    return manifest


def rename_exclusive(source, destination):
    if sys.platform == "darwin":
        rename = LIBC.renamex_np
        rename.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint)
        rename.restype = ctypes.c_int
        result = rename(os.fsencode(source), os.fsencode(destination), 4)
    elif sys.platform == "linux" and hasattr(LIBC, "renameat2"):
        rename = LIBC.renameat2
        rename.argtypes = (ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint)
        rename.restype = ctypes.c_int
        result = rename(-100, os.fsencode(source), -100, os.fsencode(destination), 1)
    else:
        fail("Host lacks atomic exclusive directory publication")
    if result:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), str(destination))


def assemble(args):
    output = args.output
    if os.path.lexists(output):
        fail("Refusing to overwrite an existing output or installation")
    for source in (args.emulator, args.manager, args.runtime_root, args.manager_source):
        if output.is_relative_to(source) or source.is_relative_to(output):
            fail("Output and input paths must not overlap")
    version, core, manager, lock, receipts = validate_inputs(args)
    output.parent.mkdir(parents=True, exist_ok=True)
    space(output.parent)
    with tempfile.TemporaryDirectory(prefix=f".{output.name}.assembling-", dir=output.parent) as directory:
        stage = Path(directory) / "payload"
        owner = uuid.uuid4().hex
        write_json(stage / MARKER, {"schema": 1, "tool": "ModAVD macOS local assembler", "owner": owner})
        stats = {"clonedFiles": 0, "copiedFiles": 0}
        for source, relative in ((args.emulator, "sdk/emulator"), (args.manager / "tools", "tools"),
                                 (args.manager / "ModAVD.app", "ModAVD.app"),
                                 (args.runtime_root / "sdk/platform-tools", "sdk/platform-tools"),
                                 (args.runtime_root / "sdk" / IMAGE, "sdk/" + IMAGE)):
            print("Assembling " + relative, flush=True)
            copy_tree(source, stage / relative, stats)
        (stage / "components").mkdir()
        extras = [(ROOT / "LICENSE", "licenses/port-LICENSE")]
        if args.ime_apk or args.launcher_apk:
            extras.append((APK_LOCK_PATH, "provenance/components.lock.json"))
        if args.ime_apk:
            extras.append((args.ime_apk, "components/modavd-ime.apk"))
        if args.launcher_apk:
            extras.append((args.launcher_apk, "components/" + LAUNCHER["filename"]))
            for key in ("licenseFile", "embeddedLicenseFile"):
                extras.append((ROOT / "ports/macos/licenses" / LAUNCHER[key], "licenses/" + LAUNCHER[key]))
        for name in ("LICENSE", "NOTICE", "SHA256SUMS.json", ".modavd-manager-build.json"):
            path = args.manager / name
            if path.is_file():
                extras.append((path, "provenance/manager/" + name))
        extras.append((ROOT / "ports/macos/runtime/components.lock.json", "provenance/runtime/components.lock.json"))
        for name in ("platform-tools", "system-image", "android-sdk-license", "android-sdk-arm-dbt-license"):
            extras.append((args.runtime_root / "receipts" / (name + ".json"), "provenance/runtime/" + name + ".json"))
        for name in ("android-sdk-license", "android-sdk-arm-dbt-license"):
            extras.append((ROOT / "ports/macos/runtime/licenses" / (name + ".txt"), "licenses/" + name + ".txt"))
        for source, relative in extras:
            target = stage / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            copy_file(source, target, stats)
        write_json(stage / "provenance/core.json", core)
        (stage / "README.zh-CN.txt").write_text(
            "ModAVD macOS ARM64\n\n双击 ModAVD.app；命令行 tools/modavd --root <本目录>。\n"
            "组件、诊断、问题包均由同一个 Go CLI 提供；不需要 Python 维护脚本。\n"
            "实例与日志存在本目录。Google Play 文件准备不代表实例装卸已开放。\n"
            "此包采用本地 ad-hoc/输入现有签名，只验证完整性；未声明 Developer ID 或公证。\n"
            + ("官方 37.2.13 仅用于机器验收，不是 ModAVD 自编核心，不能当作生产更新包。\n"
               if args.profile == "official-validation" else "自编核心身份见 provenance/core.json。\n")
            + "来源、平台、版本与逐文件摘要见 PACKAGE-MANIFEST.json；分享前检查自己生成的问题包。\n")
        write_json(stage / "modavd-manifest.json", {"schema": 1, "version": version, "build": args.build,
                                                    "platform": PLATFORM, "channel": "local-development", "profile": args.profile})
        manifest = {"schema": 2, "owner": owner, "createdAt": dt.datetime.now(dt.timezone.utc).isoformat(),
                    "platform": PLATFORM, "channel": "local-development", "version": version, "build": args.build,
                    "profile": args.profile, "emulatorProperties": core["sourceProperties"], "core": core,
                    "manager": manager, "ime": IME if args.ime_apk else {"included": False, "optional": True},
                    "launcher": LAUNCHER if args.launcher_apk else {"included": False, "optional": True},
                    "sdkComponents": {name: {"revision": receipt["revision"], "archiveSha1": receipt["archiveSha1"],
                                               "archiveSha256": receipt["archiveSha256"]} for name, receipt in receipts.items()},
                    "validation": "not_run_by_assembler", "copyStatistics": stats, "files": inventory(stage)}
        write_json(stage / MANIFEST, manifest)
        verify(stage)
        # Recheck the original receipts and clean source after the full copy too.
        validate_inputs(args)
        rename_exclusive(stage, output)
    print(json.dumps({"output": str(output), "version": version, "profile": args.profile, **stats}, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("assemble", "verify"))
    parser.add_argument("--emulator", type=Path)
    parser.add_argument("--manager", type=Path)
    parser.add_argument("--manager-source", type=Path)
    parser.add_argument("--manager-revision")
    parser.add_argument("--runtime-root", type=Path, help="Isolated root with sdk/ and verified component/license receipts/")
    parser.add_argument("--ime-apk", type=Path)
    parser.add_argument("--launcher-apk", type=Path)
    parser.add_argument("--profile", choices=("custom", "official-validation"))
    parser.add_argument("--core-receipt", type=Path, help="Custom core: source object plus complete file SHA256 map")
    parser.add_argument("--version")
    parser.add_argument("--build", type=int)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    for key, value in vars(args).items():
        if isinstance(value, Path):
            path = Path(os.path.abspath(value.expanduser()))
            setattr(args, key, path.parent.resolve() / path.name)
    if args.command == "assemble":
        for key in ("emulator", "manager", "manager_source", "manager_revision", "runtime_root", "profile", "version", "build"):
            if getattr(args, key) is None:
                parser.error("assemble requires --" + key.replace("_", "-"))
        assemble(args)
    else:
        manifest = verify(args.output)
        print(json.dumps({"ok": True, "version": manifest["version"], "profile": manifest["profile"],
                          "payloadEntries": len(manifest["files"])}))


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError, KeyError, zipfile.BadZipFile, subprocess.SubprocessError) as error:
        print(f"Package failed: {error}", file=sys.stderr)
        sys.exit(1)
