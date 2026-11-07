#!/usr/bin/env python3
"""Create a checked macOS update payload without SDK components or user state."""

from __future__ import annotations

import argparse
import base64
import ctypes
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import re
import stat
import subprocess
import sys
import tempfile
import time
import zipfile

SPEC = importlib.util.spec_from_file_location("modavd_assembler", Path(__file__).with_name("package.py"))
assembler = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(assembler)

TRANSPORT = "modavd-update-manifest.json"
DIRECTORIES = ("sdk/emulator", "ModAVD.app", "tools", "components", "licenses", "provenance")
ROOT_FILES = ("modavd-manifest.json", "README.zh-CN.txt", assembler.MARKER, assembler.MANIFEST)
SIGNATURE_XATTRS = frozenset(("com.apple.cs.CodeDirectory", "com.apple.cs.CodeRequirements",
                             "com.apple.cs.CodeSignature", "com.apple.cs.CodeEntitlements"))
MAX_FILES = 19999  # transport manifest occupies the remaining updater entry
MAX_BYTES = 4 * 1024**3


def release_identity(source, files, distribution=False):
    """Do not advertise official SDK validation builds as installable ModAVD releases."""
    version = json.loads((source / "modavd-manifest.json").read_text())
    match = re.fullmatch(r"v?(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", str(version.get("version", "")))
    if (not match or version.get("platform") != "macos-arm64"
            or type(version.get("build")) is not int or version["build"] != int(match[2])
            or version["build"] <= 0):
        raise RuntimeError("源包版本、构建号或平台无效")
    props = assembler.properties(source / "sdk/emulator/source.properties")
    if props.get("Pkg.BuildId") != "mas-" + str(version["build"]):
        raise RuntimeError("官方 SDK 验证包不能发布为 ModAVD 更新；需要版本匹配的自编核心")
    qemu = source / "sdk/emulator/qemu/darwin-aarch64/qemu-system-aarch64-headless"
    if not assembler.contains(qemu, b"free-page-reporting"):
        raise RuntimeError("自编核心缺少更新器要求的 free-page-reporting 标识")
    required = ("tools/modavd", "tools/modavd-manager", "ModAVD.app/Contents/MacOS/ModAVD",
                "sdk/emulator/emulator", "sdk/emulator/qemu/darwin-aarch64/qemu-system-aarch64",
                "sdk/emulator/qemu/darwin-aarch64/qemu-system-aarch64-headless")
    for name in required:
        assembler.arm64_executable(source / name)
    info = plistlib.loads((source / "ModAVD.app/Contents/Info.plist").read_bytes())
    if info.get("CFBundleExecutable") != "ModAVD":
        raise RuntimeError("应用入口必须是 ModAVD")
    # Signatures are verified again after streaming, before publishing the ZIP.
    deadline = time.monotonic() + 600
    for unit in ("ModAVD.app", "sdk/emulator"):
        subprocess.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(source / unit)], check=True, timeout=120)
    team = None
    for name, entry in files.items():
        path = source / name
        if entry["type"] == "file" and is_macho(path):
            if time.monotonic() > deadline:
                raise RuntimeError("代码签名总校验超过 10 分钟")
            if not assembler.macho(path):
                raise RuntimeError("原生文件缺少 ARM64 代码：" + name)
            subprocess.run(["/usr/bin/codesign", "--verify", "--strict", str(path)], check=True, timeout=30)
            if distribution:
                details = subprocess.run(["/usr/bin/codesign", "--display", "--verbose=4", str(path)],
                                         capture_output=True, text=True, check=True, timeout=30).stderr
                identity = re.search(r"(?m)^TeamIdentifier=([A-Z0-9]+)$", details)
                if "Authority=Developer ID Application:" not in details or not identity:
                    raise RuntimeError("公开发行的每个原生文件都需要 Developer ID：" + name)
                if team is None:
                    team = identity[1]
                elif team != identity[1]:
                    raise RuntimeError("公开发行原生文件的 TeamIdentifier 不一致：" + name)
    if distribution:
        app = source / "ModAVD.app"
        details = subprocess.run(["/usr/bin/codesign", "--display", "--verbose=4", str(app)],
                                 capture_output=True, text=True, check=True, timeout=30).stderr
        if "Authority=Developer ID Application:" not in details or "TeamIdentifier=not set" in details or "runtime" not in details:
            raise RuntimeError("公开发行需要 Developer ID Application 和 hardened runtime 签名")
        subprocess.run(["/usr/bin/xcrun", "stapler", "validate", str(app)], check=True, timeout=120)
        subprocess.run(["/usr/sbin/spctl", "--assess", "--type", "execute", str(app)], check=True, timeout=120)
    return version


def is_macho(path):
    with path.open("rb") as incoming:
        return incoming.read(4) in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xce", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca", b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca")


def check_paths(files, units):
    """Match updater path, case and resource limits before writing an archive."""
    folded = {}
    size = 0
    encoded_attributes = 0
    for name, entry in files.items():
        assembler.relative_name(name)
        if any(ord(c) < 32 or ord(c) > 126 for c in name):
            raise RuntimeError("更新路径必须为可打印 ASCII：" + name)
        parts = name.split("/")
        for index in range(1, len(parts) + 1):
            prefix = "/".join(parts[:index])
            previous = folded.setdefault(prefix.lower(), prefix)
            if previous != prefix:
                raise RuntimeError("更新路径大小写冲突：" + name)
            if index < len(parts) and prefix in files:
                raise RuntimeError("更新路径穿过文件或链接：" + name)
        size += entry.get("size", len(entry.get("target", "").encode("utf-8")))
        encoded_attributes += sum(len(value) for value in entry.get("xattrs", {}).values())
    if len(files) > MAX_FILES or size > MAX_BYTES or encoded_attributes > 12 * 1024**2:
        raise RuntimeError("更新负载超过更新器条目、4 GiB 或签名属性限制")
    graph = {name: item["type"] for name, item in files.items()}
    for name in files:
        parts = name.split("/")
        for index in range(1, len(parts)):
            graph.setdefault("/".join(parts[:index]), "dir")
    for name, item in files.items():
        if item["type"] != "symlink":
            continue
        unit = next(unit for unit in units if name.startswith(unit + "/"))
        stack, todo = name.split("/")[:-1], item["target"].split("/")
        floor, expansions = len(unit.split("/")), 0
        while todo:
            part = todo.pop(0)
            if part in ("", "."):
                continue
            if part == "..":
                if len(stack) <= floor:
                    raise RuntimeError("链接越过更新单元：" + name)
                stack.pop()
                continue
            prefix = "/".join(stack + [part])
            kind = graph.get(prefix)
            if kind is None:
                raise RuntimeError("更新包含悬空链接：" + name)
            if kind == "symlink":
                expansions += 1
                if expansions > 64:
                    raise RuntimeError("更新链接循环或过深：" + name)
                todo = files[prefix]["target"].split("/") + todo
            else:
                if todo and kind != "dir":
                    raise RuntimeError("更新链接穿过普通文件：" + name)
                stack.append(part)
    return size


def signature_xattrs(path):
    """ZIP lacks generic-code signature attributes; retain only these keys."""
    if sys.platform != "darwin":
        return {}
    library = ctypes.CDLL(None, use_errno=True)
    library.listxattr.argtypes = (ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int)
    library.listxattr.restype = ctypes.c_ssize_t
    library.getxattr.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p,
                                 ctypes.c_size_t, ctypes.c_uint32, ctypes.c_int)
    library.getxattr.restype = ctypes.c_ssize_t
    encoded = os.fsencode(path)
    count = library.listxattr(encoded, None, 0, 1)  # XATTR_NOFOLLOW
    if count < 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), str(path))
    if count > 65536:
        raise RuntimeError("扩展属性名称列表过长：" + path.name)
    names = ctypes.create_string_buffer(max(1, count))
    actual = library.listxattr(encoded, names, count, 1)
    if actual < 0 or actual > count:
        raise RuntimeError("读取时扩展属性发生变化：" + path.name)
    attributes = {os.fsdecode(name) for name in names.raw[:actual].split(b"\x00") if name}
    unsupported = {name for name in attributes if name.startswith("com.apple.cs.")} - SIGNATURE_XATTRS
    if unsupported:
        raise RuntimeError("发现尚未支持的代码签名属性：" + ", ".join(sorted(unsupported)))
    result = {}
    for name in sorted(attributes & SIGNATURE_XATTRS):
        size = library.getxattr(encoded, os.fsencode(name), None, 0, 0, 1)
        if size < 0 or size > 1024**2:
            raise RuntimeError("代码签名属性不可读或超过1MiB：" + path.name)
        value = ctypes.create_string_buffer(max(1, size))
        read = library.getxattr(encoded, os.fsencode(name), value, size, 0, 1)
        if read != size:
            raise RuntimeError("读取时代码签名属性发生变化：" + path.name)
        result[name] = base64.b64encode(value.raw[:size]).decode("ascii")
    return result


def record(path, unit):
    info = path.lstat()
    mode = stat.S_IMODE(info.st_mode)
    if mode & ~0o777:
        raise RuntimeError("更新负载不能含特殊权限：" + path.name)
    if path.is_symlink():
        target = os.readlink(path)
        if os.path.isabs(target) or "\\" in target or "\x00" in target:
            raise RuntimeError("更新负载含不安全链接：" + path.name)
        try:
            resolved = path.resolve(strict=True)
        except (OSError, RuntimeError) as error:
            raise RuntimeError("更新负载含悬空或循环链接：" + path.name) from error
        if not resolved.is_relative_to(unit):
            raise RuntimeError("更新链接不能跨越目录单元：" + path.name)
        return {"type": "symlink", "target": target,
                "sha256": hashlib.sha256(target.encode("utf-8")).hexdigest()}
    if not stat.S_ISREG(info.st_mode):
        raise RuntimeError("更新负载仅接受普通文件和内部链接：" + path.name)
    result = {"type": "file", "mode": mode, "size": info.st_size,
              "sha256": assembler.digest(path)}
    attributes = signature_xattrs(path)
    if attributes:
        result["xattrs"] = attributes
    return result


def collect(source):
    units, files = [], {}
    for name in DIRECTORIES:
        unit = source / name
        if not unit.exists() and name not in ("sdk/emulator", "ModAVD.app", "tools"):
            continue
        if unit.is_symlink() or not unit.is_dir():
            raise RuntimeError("缺少更新目录单元：" + name)
        before_count = len(files)
        for path in assembler.entries(unit):
            if path.is_dir() and not path.is_symlink():
                continue
            files[path.relative_to(source).as_posix()] = record(path, unit)
        if len(files) > before_count:
            units.append(name)
        elif name in ("sdk/emulator", "ModAVD.app", "tools"):
            raise RuntimeError("必需更新目录为空：" + name)
    for name in ROOT_FILES:
        path = source / name
        if path.is_symlink() or not path.is_file():
            raise RuntimeError("缺少更新根文件：" + name)
        files[name] = record(path, path.parent)
        units.append(name)
    check_paths(files, units)
    return units, dict(sorted(files.items()))


def create(source, output, distribution=False):
    source = Path(source).expanduser().resolve(strict=True)
    output = Path(output).expanduser().absolute()
    if output.is_symlink() or output.exists():
        raise RuntimeError("更新归档已存在，拒绝覆盖")
    if output.resolve().is_relative_to(source):
        raise RuntimeError("更新归档必须放在源包目录之外")
    assembler.verify(source)
    units, files = collect(source)
    version = release_identity(source, files, distribution)
    expected_name = "ModAVD-" + version["version"].removeprefix("v") + "-macos-arm64.zip"
    if output.name != expected_name:
        raise RuntimeError("附件名必须与管理器远端选择规则一致：" + expected_name)
    manifest = {"schema": 1, "format": "modavd-macos-update-v1", "platform": "macos-arm64",
                "version": version["version"], "build": version["build"], "units": units, "files": files}
    manifest_bytes = (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    if len(manifest_bytes) > 16 * 1024**2:
        raise RuntimeError("更新清单超过 16 MiB")
    if check_paths(files, units) + len(manifest_bytes) > MAX_BYTES:
        raise RuntimeError("更新负载与清单合计超过 4 GiB")
    output.parent.mkdir(parents=True, exist_ok=True)
    assembler.space(output.parent)
    fd, name = tempfile.mkstemp(prefix=".modavd-update-", suffix=".zip", dir=output.parent)
    temporary = Path(name)
    os.close(fd)
    try:
        with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True) as archive:
            archive.writestr(TRANSPORT, manifest_bytes)
            for relative, expected in files.items():
                path = source / relative
                before = path.lstat()
                entry = zipfile.ZipInfo(relative)
                entry.create_system = 3
                entry.compress_type = zipfile.ZIP_DEFLATED
                if expected["type"] == "symlink":
                    entry.external_attr = (stat.S_IFLNK | 0o777) << 16
                    data = os.readlink(path).encode("utf-8")
                    if hashlib.sha256(data).hexdigest() != expected["sha256"]:
                        raise RuntimeError("生成归档时链接发生变化：" + relative)
                    archive.writestr(entry, data)
                else:
                    entry.external_attr = (stat.S_IFREG | expected["mode"]) << 16
                    checksum = hashlib.sha256()
                    with path.open("rb") as incoming, archive.open(entry, "w", force_zip64=True) as outgoing:
                        while chunk := incoming.read(1024 * 1024):
                            assembler.space(output.parent, len(chunk))
                            checksum.update(chunk)
                            outgoing.write(chunk)
                    if checksum.hexdigest() != expected["sha256"]:
                        raise RuntimeError("生成归档时文件发生变化：" + relative)
                    if signature_xattrs(path) != expected.get("xattrs", {}):
                        raise RuntimeError("生成归档时代码签名属性发生变化：" + relative)
                after = path.lstat()
                if (before.st_ino, before.st_mode, before.st_size, before.st_mtime_ns) != (after.st_ino, after.st_mode, after.st_size, after.st_mtime_ns):
                    raise RuntimeError("生成归档时负载发生变化：" + relative)
        # Preserve the source's owner and package inventory as one update unit;
        # transport metadata is deliberately never installed in the target root.
        assembler.verify(source)
        release_identity(source, files, distribution)
        if temporary.stat().st_size > MAX_BYTES:
            raise RuntimeError("最终 ZIP 超过 4 GiB")
        checksum = assembler.digest(temporary)
        os.link(temporary, output)
        return {"output": str(output), "sha256": checksum, "bytes": output.stat().st_size,
                "files": len(files), "units": units, "version": version["version"], "build": version["build"],
                "platform": "macos-arm64", "profile": "distribution" if distribution else "local-development",
                "published": False}
    finally:
        temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--distribution", action="store_true", help="额外检查 Developer ID、hardened runtime、公证票据和 Gatekeeper")
    args = parser.parse_args()
    try:
        print(json.dumps(create(args.source, args.output, args.distribution), ensure_ascii=False, indent=2))
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, "更新归档生成失败：" + str(error) + "\n")


if __name__ == "__main__":
    main()
