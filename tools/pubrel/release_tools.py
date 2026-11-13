"""Release helpers; stdlib only. Source checkouts are always read-only.

checks.py and privacy.py are the ones in scripts/ of this repository, the same files the public CI runs.
"""
import argparse
import csv
import datetime
import hashlib
import io
import json
import lzma
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import xml.etree.ElementTree as ET
import zipfile

HERE = Path(__file__).resolve().parent
# The ModAVD checkout these tools are in (tools/pubrel -> the repository root).
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO / "scripts"))
import checks
IDENTITY = checks.MAINTAINER
# Public version X.Y.Z. Up to 0.40 it was 0.<N>.<P> from the build mas<N> (P: repack count); since then the version is
# given on its own (--version, release.ps1 -Version) and the build is only recorded next to it ("build" in the manifest
# and the machine line). Without --version the old rule still applies: mas30 and mas32 were published as
# 37.2.10-masN, which counts as 0.N.0, so builds before this one can only be repackaged (P >= 1), e.g. mas32 -> 0.32.1.
FIRST_NEW_BUILD = 33
MANIFEST = "modavd-manifest.json"
# Binary packages carry the platform (a Linux build may follow); the source bundle does not.
PLATFORM = "win-x64"
# The guest half of the host input method channel (mas-35+), shipped as components/<this name>; the name never
# changes, MAS finds it by name. It goes in only when release.ps1 gets -ImeApk.
IME_APK = "modavd-ime.apk"
# Every APK under components/, pinned by SHA-256 (see pinned_components).
COMPONENT_SUMS = HERE / "config/components.SHA256SUMS.txt"
# ModAVDExtras' capture library (LGPL-3.0-or-later, mas-39+): tools such as MAA load it from <ModAVD root>\extras\, so
# the path never changes. Every package carries it (release.ps1 -ExtrasDll): once shipped, a package without it would
# make the incremental update delete it on users' machines. Nothing about it is pinned here, as for the manager: its
# version is the one in CMakeLists.txt at the head of the release branch of the ModAVDExtras checkout (--extras-repo),
# or the tag whose checksums list it, and release/<version>.SHA256SUMS.txt at that commit must list the DLL as
# modavd_extras.dll; the source bundle archives that commit (see extras_release).
EXTRAS_DLL = "extras/modavd_extras.dll"
# How ModAVDExtras writes its version (its release workflow checks the tag against it).
EXTRAS_VERSION_FILE = "CMakeLists.txt"
EXTRAS_VERSION_RE = r"(?m)^project\(ModAVDExtras VERSION (\d+\.\d+\.\d+)"
# ModAVDExtras' MuMu compatibility shell external_renderer_ipc.dll, from the same ModAVDExtras commit as EXTRAS_DLL:
# MAA loads shell\sdk\external_renderer_ipc.dll under the MuMu path it is given, so users who run MAA on its own set
# that path to <ModAVD root>\mumu-shim (an interim measure until MAA / MaaFramework support ModAVD natively). Every
# package carries it with EXTRAS_DLL (-MumuShim); the same release/<version>.SHA256SUMS.txt lists it.
MUMU_SHIM = "mumu-shim/shell/sdk/external_renderer_ipc.dll"
# package path -> its name in ModAVDExtras' release/<version>.SHA256SUMS.txt
EXTRAS_FILES = {EXTRAS_DLL: "modavd_extras.dll", MUMU_SHIM: "external_renderer_ipc.dll"}
# The license texts the NOTICE entry of EXTRAS_DLL points to: SHA-256 with LF line endings of LICENSES/<name> in the
# license package (the texts from gnu.org; ModAVDExtras' own LICENSE / COPYING are the same licenses).
LGPL_TEXTS = {"LGPL-3.0.txt": "e3a994d82e644b03a792a930f574002658412f62407f5fee083f2555c5f23118",
              "GPL-3.0.txt": "3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986"}
# What shipped after the P3 review of the NOTICE (see extend_texts): files added to entries already there, and the
# entries appended to Part 2 (NOTICE.txt / NOTICE.csv).
NOTICE_MORE_FILES = {
    "ModAVD program icon": ["(package root) ModAVD.exe"],  # named so by ICON_NOTICE_EDITS
    "ModAVD scripts and documents": ["(package root) tools/modavd-bridge.ps1"],
}
# Files the reviewed NOTICE still names but the package no longer ships: fetching the components, collecting a problem
# report and copying the system info are in the manager and tools/modavd.exe, and the batch files and scripts that did
# them are gone (ModAVD/ModAVD#45). The readme 说明.txt is gone too: the manager and the docs site say how to use it
# (the whole-root update deletes it from a root, as a file of the old manifest the new package lacks). Each is taken out
# of its entry, where it has to be exactly once.
NOTICE_GONE_FILES = {
    "ModAVD scripts and documents": ["(package root) tools/fetch-components.ps1", "(package root) tools/collect-info.ps1",
                                     "(package root) 补全组件.bat", "(package root) 收集问题信息.bat",
                                     "(package root) 说明.txt"],
}
NOTICE_ADDITIONS = HERE / "config/notice-additions.txt"
NOTICE_ADDITIONS_CSV = HERE / "config/notice-additions.csv"
# The scripts entry of the reviewed NOTICE still speaks of batch files; the package has one .ps1 left
# (ModAVD/ModAVD#140). NOTICE.txt passages, each exactly once, and the entry's license name in NOTICE.csv.
SCRIPT_NOTICE_EDITS = [
    ("The scripts and batch files of ModAVD (the .ps1 and .bat files listed above):\n",
     "The scripts of ModAVD (the .ps1 files listed above):\n"),
    ("Source code: https://github.com/ModAVD/ModAVD/tree/main/package; the .ps1\nand .bat files listed above are "
     "themselves the source code.\n",
     "Source code: https://github.com/ModAVD/ModAVD/tree/main/package; the .ps1\nfiles listed above are themselves "
     "the source code.\n"),
    # and its documents are only the NOTICE now (说明.txt is in NOTICE_GONE_FILES)
    ("The documents of ModAVD (说明.txt, NOTICE.txt, NOTICE.csv):\n",
     "The documents of ModAVD (NOTICE.txt, NOTICE.csv):\n"),
]
SCRIPT_CSV_LICENSE = ("AGPL-3.0-or-later (scripts and batch files) / Apache-2.0 (documents)",
                      "AGPL-3.0-or-later (scripts) / Apache-2.0 (documents)")
# The program icon is ModAVD's own; the reviewed NOTICE called it fan art. Its passages that said so and what they say
# now, applied first in extend_texts, each old passage exactly once; in NOTICE.csv the entry gets ICON_ENTRY as its name
# and ICON_CSV_NOTE as its note.
ICON_ENTRY_OLD, ICON_ENTRY = "ModAVD program icon (fan art)", "ModAVD program icon"
ICON_NOTICE_EDITS = [
    (f'see "{ICON_ENTRY_OLD}" in Part 2.', f'see "{ICON_ENTRY}" in Part 2.'),
    (f"\nThe notices is included for the library: {ICON_ENTRY_OLD}\n",
     f"\nThe notices is included for the library: {ICON_ENTRY}\n"),
    ("ModAVD) is a fan-made image of the\ncharacter Eyjafjalla from the game Arknights. The character is copyright\n"
     "Shanghai Hypergryph Network Technology Co., Ltd. (Hypergryph).\n",
     "ModAVD) is ModAVD's own icon.\n"),
]
ICON_CSV_NOTE = ("Added by ModAVD: missing from the AOSP-generated notices. The program icon images are ModAVD's own icon; "
                 "not licensed under Apache-2.0, the GPL or any other license here.")
# The Go programs come from the ModAVDManager release ZIP (--manager), checked against release/<version>.SHA256SUMS.txt
# at tag v<version> of a ModAVDManager checkout (--manager-repo): path in the ZIP -> package path.
MODAVD_FILES = {"tools/modavd.exe": "tools/modavd.exe", "ModAVD.exe": "ModAVD.exe"}
MANAGER_ZIP = re.compile(r"ModAVDManager-(\d+\.\d+\.\d+)-win-x64\.zip")
# The bundled script, read from package/ of the ModAVD release checkout (--repository) at its HEAD, where package/**
# is stored byte for byte (-text), into tools/. Up to 0.38.1 the package also had three batch files at its root and
# three more scripts; what they did is in the manager and tools/modavd.exe now (ModAVD/ModAVD#45).
PACKAGE_TOOLS = ["modavd-bridge.ps1"]
# ModAVD's own programs in the package (AGPL-3.0-or-later) go into the source bundle as OWN_SOURCES_NAME: each public
# repository exactly as published, under its own folder: ModAVDManager at the tag of the shipped ZIP, ModAVDHostIME
# at the tag whose release checksums list the shipped APK, and bridge/ (the arm64 execution bridge) of the ModAVD
# release checkout at the commit the bundled scripts come from.
OWN_SOURCES_NAME = "modavd-src.tar.xz"
# Added at the root of modavd-src/ (archive name: path in this repository, read from its HEAD): the AGPL-3.0 full
# text and what in the archive is whose and where each part is published.
OWN_SOURCES_ROOT = {"LICENSE": "package/LICENSE", "NOTICE": "tools/pubrel/config/own-sources-NOTICE.txt"}
# The reviewed mas30 manifest: the template every later manifest is written from, and the old revisions that tell
# which source projects changed.
MANIFEST_MAS30 = REPO / "manifests/manifest-mas30.xml"
# SHA-256 of the AGPL-3.0 full text with LF line endings (the LICENSE file of AUTO-MAS, 34,523 bytes): the package's
# LICENSES/AGPL-3.0.txt (any line endings) and modavd-src/LICENSE (exactly) are this text.
AGPL_SHA256 = "0d96a4ff68ad6d4b6f1f30f713b18d5184912ba8dd389f86aa7710db079abcb0"
OWN_SOURCES_TEXT = """
6. ModAVD 自己的程序（AGPL-3.0-or-later，不是 AOSP 的一部分）：
   {name}   下面各公开仓库对应版本的 git archive，原样。
     根目录的 LICENSE 是 AGPL-3.0 全文；NOTICE 说明哪些是 ModAVD 自己的、各部分的公开位置，以及例外和第三方部分
     （取自 AUTO-MAS 的测试参照、管理器图标、按 AOSP 头文件声明的结构体、NDK 的 libc++、Go 运行时）。
     ModAVDManager/         ModAVD.exe（管理器）和 tools\\modavd.exe（命令行、MCP）：ModAVDManager {manager_tag}
                            （提交 {manager_commit}），即它的 Release 附件 ModAVDManager-{manager_version}-win-x64.zip
                            的源码。Go {go}，用其中的 build.ps1 编（固定 go.mod 的 toolchain、-trimpath，同一份源码
                            编出的字节相同）。https://github.com/ModAVD/ModAVDManager
{ime}{extras}     ModAVD/bridge/         sdk\\emulator\\resources\\modavd 里的 arm64 执行桥：ModAVD 仓库提交 {modavd_commit} 的
                            bridge/。libmodavd_bridge.so（nbshim/jni）、libmodavd_gate.so（companion/jni）用
                            Android NDK r27c 的 ndk-build 按 jni 下的 Android.mk / Application.mk 编（bridge/scripts）；
                            modavd_bridge.rc 是原文件。https://github.com/ModAVD/ModAVD/tree/main/bridge
   包里装执行桥的脚本（tools\\modavd-bridge.ps1）本身就是源码，随主包原样发出，不在这个归档里；
   取自同一提交的 package/：https://github.com/ModAVD/ModAVD/tree/main/package。
"""
OWN_SOURCES_IME_TEXT = """\
     ModAVDHostIME/         components\\modavd-ime.apk（宿主输入法 org.modavd.ime）：ModAVDHostIME {ime_tag}
                            （提交 {ime_commit}）。用 Android SDK build-tools 36.1.0 的 aapt2 / d8 / zipalign /
                            apksigner 和 platforms/android-36 的 android.jar 编（javac --release 17，
                            d8 --min-api 30）；签名用 ModAVD 的发布密钥，不在源码包里。
                            https://github.com/ModAVD/ModAVDHostIME
"""
OWN_SOURCES_EXTRAS_TEXT = """\
     ModAVDExtras/          extras\\modavd_extras.dll（截图库）{shim}：ModAVDExtras {extras_tag}（提交 {extras_commit}），即它的 Release 附件 modavd_extras.dll{shim_asset} 的源码。
                            按 LGPL-3.0-or-later 发布（不是 AGPL），全文是其中的 LICENSE（LGPL-3.0）和 COPYING（GPL-3.0），
                            另见其中的 NOTICE。用 CMake 和 MSVC 编（cmake -A x64，--config Release，/Brepro）。
                            https://github.com/ModAVD/ModAVDExtras
"""
# changes.md: these go above the github-only separator (in this order), the last one below it.
UPPER_SECTIONS = ["怎么更新", "AUTO-MAS 版本", "已知问题"]
LOWER_SECTIONS = ["发布前回归"]
PLACEHOLDER = checks.PLACEHOLDER
GIT_ENV = dict(os.environ, GIT_OPTIONAL_LOCKS="0", GIT_CONFIG_COUNT="2",
               GIT_CONFIG_KEY_0="core.autocrlf", GIT_CONFIG_VALUE_0="false",
               GIT_CONFIG_KEY_1="core.longpaths", GIT_CONFIG_VALUE_1="true")
# For "uncommitted changes?" only: the checkout's own line-ending settings. Under GIT_ENV's core.autocrlf=false a
# text file checked out with CRLF (core.autocrlf=true) counts as modified whenever git compares its content, which it
# does for every file of a fresh clone until the index is refreshed.
STATUS_ENV = dict(os.environ, GIT_OPTIONAL_LOCKS="0", GIT_CONFIG_COUNT="1",
                  GIT_CONFIG_KEY_0="core.longpaths", GIT_CONFIG_VALUE_0="true")


def run(*args, cwd=None, env=GIT_ENV):
    p = subprocess.run([str(a) for a in args], cwd=cwd, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode:
        raise RuntimeError(f"{args[0]} failed ({p.returncode}): {p.stderr.decode('utf-8', 'replace')[-6000:]}")
    return p.stdout


def git(repo, *args):
    return run("git", "-C", repo, "--no-optional-locks", *args).decode("utf-8").strip()


def changed(repo, *paths, untracked="all"):
    """`git status --porcelain` of `paths` (all of `repo` without any), with the checkout's own settings: empty when
    nothing there differs from HEAD."""
    return run("git", "-C", repo, "--no-optional-locks", "status", "--porcelain", f"--untracked-files={untracked}",
               "--", *paths, env=STATUS_ENV).decode("utf-8").strip()


def sha(path):
    with open(path, "rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def load(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def save(path, obj):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(json.dumps(obj, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def safe_rel(value):
    p = Path(value)
    if not value or value.startswith("/") or p.is_absolute() or re.match(r"[A-Za-z]:", value) or "\\" in value or ".." in p.parts:
        raise ValueError(f"Unsafe relative path: {value}")
    return value


def copy(src, dst):
    dst = Path(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def disk_budget(path, min_free_gb=120):
    """Free space before writing: by default a 100 GiB floor plus a 20 GiB reserve for this pipeline
    (release.ps1 -MinFreeGB / MinFreeGB in the local configuration changes it)."""
    probe = Path(path).absolute()
    while not probe.exists():
        probe = probe.parent
    free = shutil.disk_usage(probe).free
    if free < min_free_gb * 1024**3:
        raise RuntimeError(f"Need >={min_free_gb} GiB free before writing (-MinFreeGB), got {free / 1024**3:.1f}")


def inside(path, roots):
    """The first of `roots` that `path` is or lies under (links resolved, case-insensitive on Windows), else None."""
    p = Path(path).resolve()
    for root in roots or []:
        r = Path(root).resolve()
        if p == r or r in p.parents:
            return root
    return None


def no_links(path):
    p = Path(path).absolute()
    for part in (p, *p.parents):
        if part.is_symlink() or (hasattr(part, "is_junction") and part.is_junction()):
            raise RuntimeError(f"Refusing linked output path: {part}")
    if p.exists():
        for root, dirs, files in os.walk(p, followlinks=False):
            for name in dirs + files:
                child = Path(root) / name
                if child.is_symlink() or (hasattr(child, "is_junction") and child.is_junction()):
                    raise RuntimeError(f"Refusing linked output entry: {child}")


def build_number(build):
    m = re.fullmatch(r"mas([1-9][0-9]*)", build or "")
    if not m:
        raise ValueError("Invalid build number")
    return int(m[1])


def release_version(build, patch):
    """mas33 + patch 0 -> 0.33.0; mas32 + patch 1 -> 0.32.1 (no leading v; the tag adds it)."""
    number = build_number(build)
    patch = int(patch or 0)
    if not 0 <= patch <= 999:
        raise ValueError("Patch must be 0-999")
    if number < FIRST_NEW_BUILD and patch < 1:
        raise ValueError(f"{build} was published as 37.2.10-{build} (= 0.{number}.0); repackage it with -Patch 1 or later")
    return f"0.{number}.{patch}"


def public_version(args):
    """The public version of this release (no leading v): --version when given, which leaves --patch out (a nonzero
    --patch with it is refused), else release_version(--build, --patch) as up to 0.40."""
    version = getattr(args, "version", None)
    if not version:
        return release_version(args.build, getattr(args, "patch", 0))
    checks.version_key("v" + version)
    if int(getattr(args, "patch", 0) or 0):
        raise ValueError("--version gives the whole version; leave --patch out (release.ps1 -Patch)")
    return version


def package_name(version):
    """Main ZIP stem and its top-level folder: ModAVD-0.33.0-win-x64."""
    return f"ModAVD-{version}-{PLATFORM}"


def sdk_upstream(sdk):
    props = (Path(sdk) / "emulator/source.properties").read_text(encoding="utf-8-sig")
    m = re.search(r"(?m)^Pkg\.Revision=(\d+\.\d+\.\d+)\s*$", props)
    if not m:
        raise ValueError("SDK source.properties has no x.y.z Pkg.Revision")
    return m[1]


def read_changes(path):
    """Split the hand-written changes.md into its required `## ` sections."""
    text = Path(path).read_text(encoding="utf-8-sig")
    if PLACEHOLDER in text:
        raise ValueError(f"{path} still contains 「{PLACEHOLDER}」 placeholders")
    parts = re.split(r"(?m)^## (.+?)\s*$", text)
    if parts[0].strip():
        raise ValueError(f"{path}: text before the first ## heading")
    sections = {}
    for title, body in zip(parts[1::2], parts[2::2]):
        if title in sections:
            raise ValueError(f"{path}: duplicate section {title}")
        sections[title] = body.strip()
    expected = UPPER_SECTIONS + LOWER_SECTIONS
    if sorted(sections) != sorted(expected) or not all(sections.values()):
        raise ValueError(f"{path}: needs exactly these non-empty sections: {expected}")
    return sections


def read_changelog(path, version, number):
    """Entries newest first, starting at the release being built; validated with the public checks. Each build is an
    emulator build number (up to 0.40 also the middle of the version, since then counted on its own); the release's
    own entry has the build being packaged."""
    entries = load(path)
    if not isinstance(entries, list):
        raise ValueError("changelog.json must be a list")
    seen = []
    for e in entries:
        if not isinstance(e, dict) or set(e) != {"version", "build", "min_automas", "changes"}:
            raise ValueError(f"changelog entry needs exactly version/build/min_automas/changes: {e}")
        key = checks.version_key(e["version"])
        if not checks.is_build(e["build"]):
            raise ValueError(f"changelog build must be a positive integer (Pkg.BuildId mas-N): {e['version']}")
        if seen and key >= seen[-1]:
            raise ValueError("changelog.json must be sorted newest first without duplicates")
        seen.append(key)
        checks.check_min_automas(e["min_automas"])
        checks.check_changes({e["version"]: e["changes"]})
    versions = [e["version"] for e in entries]
    if "v" + version not in versions:
        raise ValueError(f"changelog.json has no entry for v{version}")
    current = entries[versions.index("v" + version)]
    if current["build"] != number:
        raise ValueError("changelog entry build differs from -Build")
    return entries[versions.index("v" + version):]


def read_sums(path):
    return parse_sums(Path(path).read_text(encoding="utf-8-sig"), path)


def parse_sums(text, where):
    result = {}
    for line in text.splitlines():
        m = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if not m or m[2] in result:
            raise ValueError(f"Invalid/duplicate checksum line in {where}: {line}")
        safe_rel(m[2])
        result[m[2]] = m[1]
    if not result:
        raise ValueError("Empty checksums")
    return result


def baseline_sums(baseline):
    """Frozen per-source checksums also support the later sources.zip layout."""
    frozen = read_sums(HERE / "config/mas30.SHA256SUMS.txt")
    expected_sources = {n: h for n, h in frozen.items() if not n.endswith(".zip")}
    for name in ["SHA256SUMS-sources.txt", "SHA256SUMS-release-assets.txt"]:
        path = Path(baseline) / name
        if path.is_file():
            candidate = read_sums(path)
            if {n: h for n, h in candidate.items() if not n.endswith(".zip")} == expected_sources:
                for n, h in candidate.items():
                    if n.endswith(".zip") and frozen.get(n) != h:
                        raise ValueError("Unexpected main ZIP in source checksum inventory")
                return frozen
    raise ValueError("Baseline source checksum inventory differs from frozen mas30 reference")


def snapshot_data(args):
    if not re.fullmatch(r"mas[1-9][0-9]*", args.build):
        raise ValueError("Invalid build number")
    data = load(HERE / "config/mas30.json")
    if args.build == "mas30":
        raise ValueError("mas30 snapshot is frozen; choose a new build number")
    manifest = ET.parse(MANIFEST_MAS30)
    for proj in manifest.getroot().findall("project"):
        repo = Path(args.emu_src) / safe_rel(proj.get("path"))
        if changed(repo, untracked="no"):
            raise RuntimeError(f"Tracked source changes must be committed before building: {proj.get('path')}")
        proj.set("revision", git(repo, "rev-parse", "HEAD"))
    data["build"] = args.build
    data["manifest_xml"] = ET.tostring(manifest.getroot(), encoding="unicode") + "\n"
    for series in data["series"]:
        series["head"] = git(Path(args.emu_src) / series["repo"], "rev-parse", "HEAD")
        series["dir"] = series["dir"].split("/")[0] + "/" + args.build
    # Release notes are written after the regression run, so they are read at release time, not frozen here.
    data.pop("changes", None)
    save(args.snapshot, data)
    print(f"Captured committed revisions for {len(manifest.getroot().findall('project'))} projects")


def pinned_components(args, pinned_path=COMPONENT_SUMS):
    """{package path: source} of the files taken from other projects' releases: the APKs that go into components/,
    checked against config/components.SHA256SUMS.txt, and EXTRAS_DLL with MUMU_SHIM, which extras_release checks
    against ModAVDExtras' own release checksums instead.

    Every APK the package carries is pinned there, and every pinned APK must come with exactly that content: once a
    file has shipped, a release without it would make the incremental update delete it on users' machines. An APK that
    is not pinned is refused, so shipping a new one (or a new build of one) is a reviewed change to that file. The two
    ModAVDExtras files come together, and with --require-extras (every real package) they must come.
    """
    pinned = read_sums(pinned_path)
    # NTFS ignores case: two names that differ only in case would be one file in the package
    if (any(not re.fullmatch(r"components/[^/]+", rel) for rel in pinned)
            or len({r.casefold() for r in pinned}) != len(pinned)):
        raise ValueError(f"{Path(pinned_path).name}: every line must be a distinct components/<file name> "
                         f"(case-insensitive); {EXTRAS_DLL} and {MUMU_SHIM} are checked against ModAVDExtras' own "
                         "release checksums, not pinned here")
    ime, extras = getattr(args, "ime_apk", None), getattr(args, "extras_dll", None)
    shim = getattr(args, "mumu_shim", None)
    if bool(shim) != bool(extras):
        raise ValueError("-ExtrasDll and -MumuShim go together: both come from the same ModAVDExtras commit")
    if not extras and getattr(args, "require_extras", False):
        raise ValueError(f"Every package carries {EXTRAS_DLL} and {MUMU_SHIM} (a release without them would make the "
                         "incremental update delete them): pass -ExtrasDll and -MumuShim")
    given = {"components/" + Path(args.apk).name: Path(args.apk)}
    if ime:
        given["components/" + IME_APK] = Path(ime)
    if len({r.casefold() for r in given}) != 1 + bool(ime):
        raise ValueError("desktop APK name collides with the IME APK")
    for rel, src in given.items():
        if not src.is_file():
            raise FileNotFoundError(src)
        if rel not in pinned:
            raise ValueError(f"{rel} is not pinned in {Path(pinned_path).name}")
        if sha(src) != pinned[rel]:
            raise ValueError(f"{rel} differs from its pinned SHA-256 ({src})")
    missing = sorted(set(pinned) - set(given))
    if missing:
        raise ValueError(f"Pinned components missing from this release (pass -ImeApk / -Apk): {missing}")
    if extras:
        given[EXTRAS_DLL] = Path(extras)
        given[MUMU_SHIM] = Path(shim)
        for src in (given[EXTRAS_DLL], given[MUMU_SHIM]):
            if not src.is_file():
                raise FileNotFoundError(src)
    return given


def checked_sources(args):
    """public_sources, and with --require-tags every component at its tag (check_tagged): what preflight and assemble
    both go by, so the snapshot cannot record an untagged component in a run that makes the draft."""
    own = public_sources(args)
    check_tagged(args, own)
    return own


def check_tagged(args, own):
    """--require-tags (release.ps1 -DraftRelease): every component is at its tag. A package built before the tags
    exist (component_ref) is published by the serial release, which tags the recorded commits first."""
    if not getattr(args, "require_tags", False):
        return
    pending = [f"{name} v{part['version']}" for name, part in own.items() if part and part.get("tagged") is False]
    if pending:
        raise ValueError("Not tagged yet: " + ", ".join(pending) + "; -DraftRelease takes released components only "
                         "(tag them first, or leave the draft to the serial release)")


def preflight(args):
    disk_budget(args.output, args.min_free_gb)
    no_links(args.output)
    data = load(args.snapshot)
    if data["build"] != args.build:
        raise ValueError("Snapshot build does not match -Build")
    manifest = ET.fromstring(data.get("manifest_xml") or MANIFEST_MAS30.read_text(encoding="utf-8"))
    revisions = {p.get("path"): p.get("revision") for p in manifest.findall("project")}
    if {s["repo"] for s in data["series"]} != {"external/qemu", "hardware/google/gfxstream", "external/crashpad"}:
        raise ValueError("Expected exactly three patched source repositories")
    for s in data["series"]:
        safe_rel(s["dir"])
        if not re.fullmatch(r"[0-9a-f]{40}", s["base"]) or not re.fullmatch(r"[0-9a-f]{40}", s["head"]) or revisions.get(s["repo"]) != s["head"]:
            raise ValueError("Patch series and manifest revisions disagree")
    version = public_version(args)
    number = build_number(args.build)
    sdk = Path(args.sdk) / "emulator"
    props = (sdk / "source.properties").read_text(encoding="utf-8-sig")
    if not re.search(rf"(?m)^Pkg.BuildId=mas-{number}\s*$", props):
        raise ValueError("SDK Pkg.BuildId differs from requested release")
    upstream = sdk_upstream(args.sdk)
    # reads and checks changes.md and changelog.json, and the release notes' length limits (step 7 checks them again)
    budget = describe_budget(*notes_budget(version, number, upstream, args.changes, args.changelog))
    baseline = Path(args.baseline)
    sums = baseline_sums(baseline)
    for name, expected in sums.items():
        if name.endswith(".zip"):
            continue
        src = baseline / "source" / name
        if sha(src) != expected:
            raise ValueError(f"Baseline asset hash differs: {name}")
    for name in ["LICENSES/NOTICE.txt", "LICENSES/NOTICE.csv"]:
        if not (Path(args.package) / name).is_file():
            raise FileNotFoundError(name)
    notice = (Path(args.package) / "LICENSES/NOTICE.txt").read_text(encoding="utf-8-sig")
    check_agpl(args.package)
    components = pinned_components(args)
    lgpl = any(rel in EXTRAS_FILES for rel in components)
    if lgpl:
        check_lgpl(args.package)
    additions = NOTICE_ADDITIONS.read_text(encoding="utf-8")
    for rel in components:
        # the entry is in the reviewed license package, or among the entries assemble adds (config/notice-additions.txt)
        if f"(package root) {rel}" not in notice and f"\n(package root) {rel}\n" not in additions:
            raise ValueError(f"LICENSES/NOTICE.txt has no entry for {rel}")
    # modavd-src takes LICENSE and NOTICE from HEAD
    if changed(REPO, *OWN_SOURCES_ROOT.values()):
        raise ValueError("Uncommitted changes under " + ", ".join(OWN_SOURCES_ROOT.values()) + "; commit first")
    own = checked_sources(args)
    print(f"Preflight OK: v{version} (mas-{number}, Emulator {upstream}), changes, changelog, release notes ({budget}), snapshot, baseline hashes, "
          f"AGPL-3.0 full text{', LGPL-3.0 / GPL-3.0 full texts' if lgpl else ''}, "
          f"pinned components ({', '.join(sorted(Path(r).name for r in components))}), disk budget, output path; "
          f"{describe_sources(own)}")


def tagged(repo, tag):
    """The commit of `tag` in a checkout of a public repository."""
    try:
        return git(repo, "rev-parse", "--verify", "--quiet", tag + "^{commit}")
    except RuntimeError:
        raise ValueError(f"{repo} has no tag {tag}; fetch the tags first (git -C {repo} fetch upstream --tags)") from None


def go_toolchain(gomod):
    """The toolchain line of a go.mod (go1.26.5): the only Go build.ps1 builds with, so the one the NOTICE names."""
    m = re.search(r"^toolchain\s+(go\S+)\s*$", gomod, re.M)
    if not m:
        raise ValueError("go.mod has no toolchain line")
    return m[1]


def component_ref(repo, version, branch=None, version_file=None, version_pattern=None):
    """(commit, where, tagged) of release `version` of a component in the checkout `repo`.

    Released: tag v<version>. Not tagged yet, and `branch` given (release/<X>.<Y> of the ModAVD version): the head of the
    remote-tracking refs/remotes/upstream/<branch>, which must already be that version (`version_pattern` finds it in
    `version_file`, as the component's release workflow checks at the tag). release/<version>.SHA256SUMS.txt is
    committed before the tag in both cases, so the files are checked the same way; the package records the commit, and
    the serial release puts the tag on exactly that commit before it publishes."""
    tag = "v" + version
    try:
        return git(repo, "rev-parse", "--verify", "--quiet", f"refs/tags/{tag}^{{commit}}"), tag, True
    except RuntimeError:
        pass
    if not branch:
        raise ValueError(f"{repo} has no tag {tag}; fetch the tags first (git -C {repo} fetch upstream --tags)")
    ref = f"refs/remotes/upstream/{branch}"
    try:
        commit = git(repo, "rev-parse", "--verify", "--quiet", ref + "^{commit}")
    except RuntimeError:
        raise ValueError(f"{repo} has neither tag {tag} nor {ref}; fetch first (git -C {repo} fetch upstream --tags)") from None
    try:
        found = re.findall(version_pattern, git(repo, "show", f"{commit}:{version_file}"))
    except RuntimeError:
        found = []
    if found != [version]:
        raise ValueError(f"{repo}: {version_file} at {branch} ({commit[:12]}) says {found or 'no version'}, not {version}")
    return commit, commit, False


def manager_release(zip_path, repo, branch=None):
    """The ModAVDManager release ZIP (ModAVDManager-<version>-win-x64.zip, from its GitHub Release or built from the
    release branch): the ZIP and the programs in it are the ones release/<version>.SHA256SUMS.txt lists at tag
    v<version> of the checkout `repo`, or before that tag exists at the head of `branch` (component_ref)."""
    path = Path(zip_path or "")
    m = MANAGER_ZIP.fullmatch(path.name)
    if not m:
        raise ValueError(f"--manager must be ModAVDManager-<version>-win-x64.zip (its GitHub Release asset, or built from its release branch), got {zip_path!r}")
    if not path.is_file():
        raise FileNotFoundError(path)
    version = m[1]
    commit, tag, is_tagged = component_ref(repo, version, branch, "internal/buildinfo/buildinfo.go",
                                           r'(?m)^(?:var|const)\s+Version\s*=\s*"([^"]+)"')
    try:
        # read at the commit, not by name: the name is only for messages
        listed = parse_sums(git(repo, "show", f"{commit}:release/{version}.SHA256SUMS.txt"), f"{tag}:release/{version}.SHA256SUMS.txt")
    except RuntimeError:
        raise ValueError(f"{repo}: no release/{version}.SHA256SUMS.txt at {tag}") from None
    got = {path.name: sha(path)}
    with zipfile.ZipFile(path) as z:
        for name in MODAVD_FILES:
            try:
                got[name] = hashlib.sha256(z.read(name)).hexdigest()
            except KeyError:
                raise ValueError(f"{path.name} has no {name}") from None
    for name, digest in got.items():
        if listed.get(name) != digest:
            raise ValueError(f"{name} in {path} is not the published one: SHA-256 {digest}, "
                             f"release/{version}.SHA256SUMS.txt at {tag} lists {listed.get(name)}")
    return {"repo": str(Path(repo).resolve()), "version": version, "tag": "v" + version, "commit": commit, "tagged": is_tagged,
            "go": go_toolchain(git(repo, "show", f"{commit}:go.mod"))}


def ime_release(apk, repo):
    """The ModAVDHostIME tag whose release/<version>.SHA256SUMS.txt lists this APK as modavd-ime.apk."""
    digest = sha(apk)
    for tag in git(repo, "tag", "--list", "v*").split():
        version = tag[1:]
        try:
            text = git(repo, "show", f"{tag}:release/{version}.SHA256SUMS.txt")
        except RuntimeError:
            continue
        if parse_sums(text, f"{tag}:release/{version}.SHA256SUMS.txt").get(IME_APK) == digest:
            return {"repo": str(Path(repo).resolve()), "version": version, "tag": tag, "commit": tagged(repo, tag)}
    raise ValueError(f"No tag of {repo} lists {Path(apk).name} (SHA-256 {digest}) as {IME_APK} in release/<version>.SHA256SUMS.txt; "
                     f"use the APK of a published release and fetch the tags (git -C {repo} fetch upstream --tags)")


def extras_version(dll, repo, branch=None):
    """The ModAVDExtras version this release ships, read from `repo` (nothing in ModAVD pins it): when `branch` (the
    release branch, release/<X>.<Y> of the ModAVD version) exists as refs/remotes/upstream/<branch>, the version in
    EXTRAS_VERSION_FILE at its head, tagged or not (so a DLL of an older version is refused there); otherwise the tag
    v<version> whose release/<version>.SHA256SUMS.txt lists this DLL as modavd_extras.dll (as ime_release finds the
    IME)."""
    head = None
    if branch:
        try:
            head = git(repo, "rev-parse", "--verify", "--quiet", f"refs/remotes/upstream/{branch}^{{commit}}")
        except RuntimeError:
            pass
    if head:
        try:
            found = re.findall(EXTRAS_VERSION_RE, git(repo, "show", f"{head}:{EXTRAS_VERSION_FILE}"))
        except RuntimeError:
            found = []
        if len(found) != 1:
            raise ValueError(f"{repo}: {EXTRAS_VERSION_FILE} at {branch} ({head[:12]}) says {found or 'no version'}")
        return found[0]
    digest = sha(dll)
    for tag in git(repo, "tag", "--list", "v*").split():
        try:
            text = git(repo, "show", f"{tag}:release/{tag[1:]}.SHA256SUMS.txt")
        except RuntimeError:
            continue
        if parse_sums(text, f"{tag}:release/{tag[1:]}.SHA256SUMS.txt").get(EXTRAS_FILES[EXTRAS_DLL]) == digest:
            return tag[1:]
    raise ValueError(f"No tag of {repo} lists {Path(dll).name} (SHA-256 {digest}) as modavd_extras.dll in "
                     f"release/<version>.SHA256SUMS.txt; use the DLL of a published release and fetch the tags "
                     f"(git -C {repo} fetch upstream --tags), or fetch its release branch (--release-branch)")


def extras_release(dll, repo, branch=None, shim=None):
    """The ModAVDExtras source of the shipped EXTRAS_DLL (and MUMU_SHIM, `shim`) in the checkout `repo`: the version
    extras_version reads, at tag v<version> or before that tag exists the head of `branch` (component_ref).
    release/<version>.SHA256SUMS.txt at that one commit lists this DLL as modavd_extras.dll and the shell as
    external_renderer_ipc.dll, so both are of the same ModAVDExtras version and commit: the workflows there build them
    and check them against that list (another MSVC gives other bytes, so the files are the ones CI built, not a local
    build)."""
    if not repo:
        raise ValueError("--extras-dll needs the ModAVDExtras checkout (release.ps1 -ExtrasRepo, local configuration ExtrasRepo)")
    version = extras_version(dll, repo, branch)
    commit, tag, is_tagged = component_ref(repo, version, branch, EXTRAS_VERSION_FILE, EXTRAS_VERSION_RE)
    where = f"{tag}:release/{version}.SHA256SUMS.txt"
    try:
        listed = parse_sums(git(repo, "show", f"{commit}:release/{version}.SHA256SUMS.txt"), where)
    except RuntimeError:
        raise ValueError(f"{repo}: no release/{version}.SHA256SUMS.txt at {tag}") from None
    files = {}
    for rel, path in [(EXTRAS_DLL, dll), (MUMU_SHIM, shim)]:
        if not path:
            continue
        digest, name = sha(path), EXTRAS_FILES[rel]
        if listed.get(name) != digest:
            raise ValueError(f"{path} is not the published {name}: SHA-256 {digest}, {where} lists {listed.get(name)}")
        files[rel] = digest
    return {"repo": str(Path(repo).resolve()), "version": version, "tag": "v" + version, "commit": commit,
            "tagged": is_tagged, "sha256": files[EXTRAS_DLL], "files": files}


def public_sources(args):
    """What the package takes from the public repositories, checked: the manager ZIP against the checksums at its
    tag, the IME APK against the tag that lists it, the extras DLL against the checksums of the ModAVDExtras version
    extras_version reads, and package/ and bridge/ of the ModAVD release checkout unchanged since its HEAD. With --release-branch the
    manager and the extras library may also be at the head of that branch of their repository, before their tags
    exist (component_ref). assemble records the result in the snapshot, so every later step uses the same revisions."""
    extras, shim = getattr(args, "extras_dll", None), getattr(args, "mumu_shim", None)
    if shim and not extras:
        raise ValueError("-MumuShim goes with -ExtrasDll: both come from the same ModAVDExtras release")
    branch = getattr(args, "release_branch", None)
    own = {"manager": manager_release(args.manager, args.manager_repo, branch),
           "ime": ime_release(args.ime_apk, args.ime_repo) if getattr(args, "ime_apk", None) else None,
           "extras": extras_release(extras, getattr(args, "extras_repo", None), branch, shim) if extras else None}
    repo = release_repository(args.repository, getattr(args, "protect", None))
    if changed(repo, "package", "bridge"):
        raise ValueError(f"{repo}: uncommitted changes under package/ or bridge/; the package takes both from HEAD")
    commit = git(repo, "rev-parse", "HEAD")
    for rel in [*("package/" + n for n in PACKAGE_TOOLS), "bridge/nbshim/modavd_bridge.rc"]:
        try:
            git(repo, "cat-file", "-e", f"{commit}:{rel}")
        except RuntimeError:
            raise ValueError(f"{repo}: no {rel} at HEAD {commit[:12]}") from None
    own["modavd"] = {"repo": str(repo), "commit": commit}
    return own


def describe_sources(own):
    def at(part):
        return part["tag"] if part.get("tagged", True) else f"{part['tag']} (NOT TAGGED YET: release branch head {part['commit'][:12]})"
    ime = f", ModAVDHostIME {own['ime']['tag']}" if own["ime"] else ""
    extras = (f", ModAVDExtras {at(own['extras'])}{' (with the MuMu shell)' if MUMU_SHIM in own['extras'].get('files', {}) else ''}"
              if own.get("extras") else "")
    return (f"ModAVDManager {at(own['manager'])} (Go {own['manager']['go']}){ime}{extras}, "
            f"ModAVD package/ and bridge/ at {own['modavd']['commit'][:12]}")


def check_agpl(package):
    """ModAVD's own programs and scripts are AGPL-3.0-or-later, so the license package must carry the full text as
    LICENSES/AGPL-3.0.txt (the NOTICE points there). Line endings do not matter (the P3 texts are CRLF)."""
    path = Path(package) / "LICENSES/AGPL-3.0.txt"
    if not path.is_file():
        raise FileNotFoundError(f"{path} is missing: ModAVD's own programs are AGPL-3.0-or-later and the NOTICE points "
                                "to this full text; use a license package that has it (-Package)")
    got = hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
    if got != AGPL_SHA256:
        raise ValueError(f"{path} is not the AGPL-3.0 full text: SHA-256 with LF line endings is {got}, expected {AGPL_SHA256}")


def check_lgpl(package):
    """The extras DLL is LGPL-3.0-or-later, and its NOTICE entry points to LICENSES/LGPL-3.0.txt and the GPL-3.0 it
    supplements, LICENSES/GPL-3.0.txt: the license package must carry both, as LGPL_TEXTS (any line endings)."""
    for name, expected in LGPL_TEXTS.items():
        path = Path(package) / "LICENSES" / name
        if not path.is_file():
            raise FileNotFoundError(f"{path} is missing: {EXTRAS_DLL} is LGPL-3.0-or-later and its NOTICE entry points to "
                                    "LICENSES/LGPL-3.0.txt and LICENSES/GPL-3.0.txt; use a license package that has them (-Package)")
        got = hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
        if got != expected:
            raise ValueError(f"{path} is not the {name[:-4]} full text: SHA-256 with LF line endings is {got}, expected {expected}")


def owned(args, create=False):
    no_links(args.output)
    marker = Path(args.output) / ".modavd-release.json"
    expected = {"owner": "modavd-release-v1", "path": str(Path(args.output).resolve()), "build": args.build}
    if create:
        save(marker, expected)
    elif load(marker) != expected:
        raise ValueError("Output ownership marker differs; refusing deletion")


def assemble(args):
    out = Path(args.output)
    data = load(args.snapshot)
    version = public_version(args)
    number = build_number(args.build)
    current =read_changelog(args.changelog, version, number)[0]
    own = checked_sources(args)
    # Later steps take the public version and the public sources from the output copy of the snapshot.
    data.update(version=version, patch=checks.version_key("v" + version)[2], upstream=sdk_upstream(args.sdk), min_automas=current["min_automas"],
                own=own)
    save(out / "snapshot.json", data)
    sdk = Path(args.sdk) / "emulator"
    name = package_name(version)
    dest = out / "release" / name
    rows = list(csv.DictReader((HERE / "config/sdk-inventory.csv").open(encoding="utf-8-sig", newline="")))
    expected = {r["文件"] for r in rows}
    actual = {p.relative_to(sdk).as_posix() for p in sdk.rglob("*") if p.is_file()}
    if expected != actual:
        raise ValueError(f"SDK inventory changed; review packaging policy first. Missing={sorted(expected-actual)}, added={sorted(actual-expected)}")
    removed = []
    for r in rows:
        rel = safe_rel(r["文件"])
        if "建议拿掉" in r["结论"] or "可选拿掉" in r["结论"]:
            removed.append(rel)
        else:
            copy(sdk / rel, dest / "sdk/emulator" / rel)
    (out / "removed-files.txt").write_text("\n".join(sorted(removed)) + "\n", encoding="utf-8")
    copy_public_files(own, args.manager, dest)
    copy_pinned_components(args, own, dest)
    for p in (Path(args.package) / "LICENSES").iterdir():
        if p.is_file():
            copy(p, dest / "LICENSES" / p.name)
    # The reviewed P3 texts say "37.2.10-mas30": the folder name becomes ModAVD-<version>-<platform>, the product
    # name the new version; the build ID and manifest names stay masN.
    for text in (dest / "LICENSES").glob("NOTICE.*"):
        content = text.read_text(encoding="utf-8-sig").replace("ModAVD-37.2.10-mas30", name).replace("37.2.10-mas30", version)
        content = content.replace("mas-30", f"mas-{number}").replace("mas30", args.build)
        text.write_text(content, encoding="utf-8")
    extend_texts(dest, own["manager"]["go"])
    check_notice_coverage(dest)
    for name0 in ["NOTICE.txt", "NOTICE.csv"]:
        copy(dest / "LICENSES" / name0, dest / "sdk/emulator" / name0)
    write_manifest(dest, data)
    print(f"Assembled {sum(p.is_file() for p in dest.rglob('*'))} files including {MANIFEST}; excluded {len(removed)}; "
          f"{describe_sources(own)}")


def copy_pinned_components(args, own, dest, pinned_path=COMPONENT_SUMS):
    """Into the package tree `dest`, at their package paths: the files pinned_components checks. Each is checked again
    after the copy, as the source may have been rebuilt in between: the ModAVDExtras files against what public_sources
    recorded for them (`own`), the APKs against their pinned SHA-256."""
    pinned = read_sums(pinned_path)
    for rel, src in pinned_components(args, pinned_path).items():
        copy(src, dest / rel)
        want = own["extras"]["files"][rel] if rel in EXTRAS_FILES else pinned[rel]
        if sha(dest / rel) != want:
            raise ValueError(f"{rel} in the package differs from its pinned SHA-256")


def copy_public_files(own, manager, dest):
    """Into the package tree `dest`: the bundled script, byte for byte from package/ at the recorded ModAVD commit,
    and the programs from the manager ZIP (both checked by public_sources)."""
    scripts = {"tools/" + n: n for n in PACKAGE_TOOLS}
    for rel, name in scripts.items():
        (dest / rel).parent.mkdir(parents=True, exist_ok=True)
        (dest / rel).write_bytes(run("git", "-C", own["modavd"]["repo"], "--no-optional-locks", "cat-file", "blob",
                                     f"{own['modavd']['commit']}:package/{name}"))
    with zipfile.ZipFile(manager) as z:
        for built, rel in MODAVD_FILES.items():
            (dest / rel).parent.mkdir(parents=True, exist_ok=True)
            (dest / rel).write_bytes(z.read(built))


def render_csv(rows):
    buf = io.StringIO()
    csv.writer(buf, lineterminator="\n").writerows(rows)  # as p3/gen_notice.py wrote it
    return buf.getvalue()


def extend_texts(dest, go):
    """Bring the P3-reviewed NOTICE.txt / NOTICE.csv up to what this package ships: ICON_NOTICE_EDITS
    (and the icon entry's name and note in NOTICE.csv), SCRIPT_NOTICE_EDITS (and SCRIPT_CSV_LICENSE), NOTICE_GONE_FILES out of their entries, NOTICE_MORE_FILES into entries already there, config/notice-additions.* at the end of Part 2
    (`go`: the Go version the manager was built with). Every anchor has to be there exactly once."""
    path = dest / "LICENSES/NOTICE.txt"
    text = path.read_text(encoding="utf-8-sig")
    for old, new in ICON_NOTICE_EDITS + SCRIPT_NOTICE_EDITS:
        if text.count(old) != 1:
            raise ValueError(f"NOTICE.txt: the passage {old.strip().splitlines()[0]!r} not found exactly once")
        text = text.replace(old, new)
    for lib, files in NOTICE_GONE_FILES.items():
        anchor = f"\nThe notices is included for the library: {lib}\n"
        if text.count(anchor) != 1:
            raise ValueError(f"NOTICE.txt: entry {lib!r} not found exactly once")
        for name in files:
            # the entry's own file list: from its "Notices for file(s):" line to its anchor
            end = text.index(anchor)
            start = text.rfind("\nNotices for file(s):\n", 0, end)
            line = f"\n{name}\n"
            if start < 0 or text.count(line) != 1 or not start < text.index(line) < end:
                raise ValueError(f"NOTICE.txt: {name!r} not found exactly once in entry {lib!r}")
            text = text.replace(line, "\n")
    for lib, files in NOTICE_MORE_FILES.items():
        anchor = f"\nThe notices is included for the library: {lib}\n"
        if text.count(anchor) != 1:
            raise ValueError(f"NOTICE.txt: entry {lib!r} not found exactly once")
        text = text.replace(anchor, "\n" + "\n".join(files) + anchor)
    part3 = list(re.finditer(r"^#{20,}\n# PART 3\. ", text, re.M))
    if len(part3) != 1:
        raise ValueError("NOTICE.txt: the Part 3 heading not found exactly once")
    additions = NOTICE_ADDITIONS.read_text(encoding="utf-8").replace("{{GO_VERSION}}", go).rstrip("\n") + "\n\n"
    path.write_text(text[:part3[0].start()] + additions + text[part3[0].start():], encoding="utf-8")

    path = dest / "LICENSES/NOTICE.csv"
    original = path.read_text(encoding="utf-8-sig")
    rows = list(csv.reader(io.StringIO(original)))
    if render_csv(rows) != original:
        raise ValueError("NOTICE.csv does not read back the way it was written; refusing to rewrite it")
    name_col, files_col = rows[0].index("External Library Name"), rows[0].index("Binaries using the license")
    hits = [r for r in rows[1:] if r[name_col] == ICON_ENTRY_OLD]
    if len(hits) != 1:
        raise ValueError(f"NOTICE.csv: entry {ICON_ENTRY_OLD!r} not found exactly once")
    hits[0][name_col], hits[0][rows[0].index("ModAVD note")] = ICON_ENTRY, ICON_CSV_NOTE
    license_col = rows[0].index("License Name")
    hits = [r for r in rows[1:] if r[license_col] == SCRIPT_CSV_LICENSE[0]]
    if len(hits) != 1:
        raise ValueError(f"NOTICE.csv: license {SCRIPT_CSV_LICENSE[0]!r} not found exactly once")
    hits[0][license_col] = SCRIPT_CSV_LICENSE[1]
    for lib, files in NOTICE_GONE_FILES.items():
        hits = [r for r in rows[1:] if r[name_col] == lib]
        if len(hits) != 1:
            raise ValueError(f"NOTICE.csv: entry {lib!r} not found exactly once")
        # the field is the names joined by single spaces, and a "(package root) <path>" name has a space of its own
        field = " " + hits[0][files_col] + " "
        for name in files:
            if field.count(f" {name} ") != 1:
                raise ValueError(f"NOTICE.csv: {name!r} not found exactly once in entry {lib!r}")
            field = field.replace(f" {name} ", " ")
        hits[0][files_col] = field[1:-1]
    for lib, files in NOTICE_MORE_FILES.items():
        hits = [r for r in rows[1:] if r[name_col] == lib]
        if len(hits) != 1:
            raise ValueError(f"NOTICE.csv: entry {lib!r} not found exactly once")
        hits[0][files_col] += " " + " ".join(files)
    added = list(csv.reader(io.StringIO(NOTICE_ADDITIONS_CSV.read_text(encoding="utf-8").replace("{{GO_VERSION}}", go))))
    if added[0] != rows[0]:
        raise ValueError("config/notice-additions.csv: header differs from NOTICE.csv")
    path.write_text(render_csv(rows + added[1:]), encoding="utf-8")


def check_notice_coverage(dest):
    """Every file at the package root and below (not sdk/) is named in NOTICE.txt as "(package root) <path>", and so
    is every file of ours under sdk/emulator/resources/modavd/ (as "./<path>"); the license texts excepted. The other
    way round, every "(package root) <path>" is a file of the package. Runs before the manifest exists."""
    notice = (dest / "LICENSES/NOTICE.txt").read_text(encoding="utf-8-sig")
    named = set(re.findall(r"^\(package root\) (.+)$", notice, re.M))
    named_sdk = set(re.findall(r"^\./(.+)$", notice, re.M))
    gone = sorted(n for n in named if not (Path(dest) / n).is_file())
    if gone:
        raise ValueError("LICENSES/NOTICE.txt names files the package does not have: " + ", ".join(gone))
    missing = []
    for p in sorted(Path(dest).rglob("*")):
        rel = p.relative_to(dest).as_posix()
        if not p.is_file() or (rel.startswith("LICENSES/") and rel not in ("LICENSES/NOTICE.txt", "LICENSES/NOTICE.csv")):
            continue
        if rel.startswith("sdk/"):
            sub = rel.removeprefix("sdk/emulator/")
            if sub.startswith("resources/modavd/") and sub not in named_sdk:
                missing.append(rel)
        elif rel not in named:
            missing.append(rel)
    if missing:
        raise ValueError("LICENSES/NOTICE.txt has no entry for: " + ", ".join(missing))


def write_manifest(tree, data):
    """Per-file size and SHA-256 of the package; the updater checks the installed result against it."""
    files = {}
    for p in sorted(Path(tree).rglob("*")):
        rel = p.relative_to(tree).as_posix()
        if p.is_file() and rel != MANIFEST:
            files[rel] = {"size": p.stat().st_size, "sha256": sha(p)}
    manifest = {"schema": 1, "version": "v" + data["version"], "build": build_number(data["build"]),
                "upstream": data["upstream"], "platform": PLATFORM, "min_automas": data["min_automas"], "files": files}
    save(Path(tree) / MANIFEST, manifest)
    checks.check_manifest(tree)


def map_files(root):
    return {p.relative_to(root).as_posix(): sha(p) for p in root.rglob("*") if p.is_file()}


def release_repository(value, protected=()):
    """The ModAVD release checkout, refused inside any of `protected` (source, backup and production
    locations: --protect, which release.ps1 fills from -EmuSrc, -Sdk and ProtectedRoots of the local configuration)."""
    repo = Path(value).resolve()
    hit = inside(repo, protected)
    if hit:
        raise ValueError(f"Refusing manifest writes in source, backup or production locations ({hit})")
    if not (repo / ".git").exists() or not (repo / "patches/BASE.md").is_file():
        raise ValueError("Repository must be an existing ModAVD checkout with patches/BASE.md")
    if "ModAVD" not in (repo / "README.md").read_text(encoding="utf-8-sig"):
        raise ValueError("Repository README does not identify ModAVD")
    return repo


def zip_members(archive, prefix):
    """SHA-256 of every file entry, keyed by its path below `prefix` ("" for a ZIP without a top-level folder)."""
    got = {}
    with zipfile.ZipFile(archive) as z:
        for e in z.infolist():
            # ZipInfo.filename normalizes backslashes on Windows; orig_filename preserves raw separators.
            path = e.orig_filename
            safe_rel(path)
            if not path.startswith(prefix):
                raise ValueError(f"Wrong ZIP top level: {path}")
            if any(ord(c) > 127 for c in path) and not e.flag_bits & 0x800:
                raise ValueError(f"UTF-8 flag missing: {path}")
            if e.is_dir():
                continue
            rel = path[len(prefix):]
            if rel in got:
                raise ValueError(f"Duplicate ZIP entry: {path}")
            with z.open(e) as f:
                got[rel] = hashlib.file_digest(f, "sha256").hexdigest()
    return got


def verify_zip(args):
    out = Path(args.output)
    name = package_name(load(out / "snapshot.json")["version"])
    tree = out / "release" / name
    expected = map_files(tree)
    removed = (out / "removed-files.txt").read_text(encoding="utf-8").splitlines()
    # The package root is the ModAVD root (no versioned top-level folder): MirrorChyan diffs files by path.
    archive = out / (name + ".zip")
    got = zip_members(archive, "")
    if expected != got:
        raise ValueError(f"{archive.name}: file set or contents differ from release directory (a top-level folder is not allowed)")
    if any("sdk/emulator/" + r in got for r in removed):
        raise ValueError(f"{archive.name}: excluded SDK file is present")
    checks.check_manifest(tree)
    save(out / "logs/verify-zip.json", {"files": len(expected), "hash_differences": 0, "excluded": len(removed), "utf8": True, "top_level_folder": False})
    print(f"ZIP verified: {len(expected)} files at the package root; slash paths; UTF-8 flags; excluded files absent; manifest matches")


def patch_header(header, keep):
    """One format-patch header with Co-authored-by trailers dropped and the From: header written as IDENTITY,
    unless |keep| (an outside contributor's noreply authorship, published as is)."""
    lines, mail, in_from = [], True, False
    for line in header.split(b"\n"):
        if mail and in_from and line[:1] in (b" ", b"\t"):
            # The rest of a folded (long or encoded) From: header.
            if keep:
                lines.append(line)
            continue
        in_from = mail and line.startswith(b"From: ")
        mail = mail and line.strip() != b""
        if in_from and not keep:
            line = b"From: " + IDENTITY.encode()
        elif re.search(rb"(?i)^co-authored-by:|noreply@anthropic\.com", line):
            continue
        lines.append(line)
    return re.sub(rb"\n{3,}$", b"\n\n", b"\n".join(lines))


def upstream_pick(repo, commit):
    """Someone else's commit taken with `git cherry-pick -x`: its message names the upstream commit, and its author
    is not whoever committed it here. The maintainer's own cherry-picks (author = committer) are not."""
    message = git(repo, "log", "-1", "--format=%B", commit)
    author_email, committer_email = git(repo, "log", "-1", "--format=%ae%n%ce", commit).split("\n")
    return bool(checks.UPSTREAM_PICK.search(message)) and author_email.strip().lower() != committer_email.strip().lower()


def export_patches(args):
    out = Path(args.output)
    data = load(out / "snapshot.json")
    series_log = []
    for series in data["series"]:
        repo = Path(args.emu_src) / safe_rel(series["repo"])
        base, head = series["base"], series["head"]
        git(repo, "merge-base", "--is-ancestor", base, head)
        raw = out / "work/raw" / safe_rel(series["dir"])
        dest = out / "patches" / safe_rel(series["dir"])
        raw.mkdir(parents=True)
        dest.mkdir(parents=True)
        git(repo, "format-patch", "--no-signature", "-o", str(raw), f"{base}..{head}")
        files = sorted(raw.glob("*.patch"))
        originals = git(repo, "rev-list", "--reverse", f"{base}..{head}").splitlines()
        if len(files) != len(originals) or not files:
            raise ValueError("Nonlinear or empty patch series")
        authors = []
        for f, commit in zip(files, originals):
            # Outside contributors keep their GitHub noreply authorship, and upstream authors keep theirs on a
            # commit cherry-picked from upstream; any other identity in the local branches (other addresses,
            # worker accounts, the maintainer's own cherry-picks) is published as IDENTITY.
            original = git(repo, "log", "-1", "--format=%an <%ae>", commit)
            upstream = upstream_pick(repo, commit)
            keep = bool(checks.NOREPLY_AUTHOR.fullmatch(original)) or upstream
            if keep and not checks.am_keeps_name(original):
                raise ValueError(f"{commit}: git am would drop the author name; amend it to 3-60 bytes without @")
            if upstream and not checks.author_allowed(original, git(repo, "log", "-1", "--format=%B", commit)):
                raise ValueError(f"{commit}: cherry-picked upstream author {original!r} cannot be published as is")
            authors.append(original if keep else IDENTITY)
            payload = f.read_bytes()
            separator = re.search(rb"(?m)^---\r?$", payload)
            if not separator:
                raise ValueError(f"Missing patch separator: {f.name}")
            header, diff = payload[:separator.start()], payload[separator.start():]
            header = patch_header(header, keep)
            mail = header.split(b"\n\n", 1)[0]
            if keep:
                author = b"<" + original.rsplit("<", 1)[1].encode() in mail
            else:
                author = re.search(rb"(?m)^From: " + re.escape(IDENTITY.encode()) + rb"$", mail)
            if len(re.findall(rb"(?m)^From: ", mail)) != 1 or not author or re.search(rb"(?i)co-authored-by:", header):
                raise ValueError(f"Patch author/trailer check failed: {f.name}")
            (dest / f.name).write_bytes(header + diff)
        entry = dict(series, count=len(files), tree=git(repo, "rev-parse", head + "^{tree}"),
                     trees=[git(repo, "rev-parse", c + "^{tree}") for c in originals], authors=authors,
                     path="patches/" + series["dir"])
        series_log.append(entry)
        print(f"Exported {series['dir']}: {len(files)} patches")
    published = published_patches(data)
    if published:
        # The release checkout gets its own series.json and BASE.md back with this release added, not replaced by a
        # summary of this build (0.38.0 lost the older rows and the license section that way). A series that is
        # already published keeps its directory: its export moves out of patches/, so only new directories are
        # there to copy into the checkout.
        records, text, dirs = merge_patch_records(series_log, *published, out, data["version"], build_number(data["build"]))
        for s in series_log:
            if not dirs[s["repo"]]["new"]:
                moved = out / "work/patches-reused" / safe_rel(s["dir"])
                moved.parent.mkdir(parents=True, exist_ok=True)
                (out / s["path"]).rename(moved)
                s["path"] = moved.relative_to(out).as_posix()
        for d in sorted((out / "patches").iterdir()):
            if d.is_dir() and not any(d.iterdir()):
                d.rmdir()
        save(out / "patches/series.json", records)
        (out / "patches/BASE.md").write_text(text, encoding="utf-8", newline="\n")
        save(out / "logs/patch-dirs.json", dirs)
        print("Patch directories: " + "; ".join(f"{r} -> {d['dir']} ({'new' if d['new'] else 'already published'})"
                                                 for r, d in dirs.items()))
        save(out / "logs/patch-series.json", series_log)
        return
    save(out / "patches/series.json", [{k: v for k, v in s.items() if k not in ("trees", "authors", "path")} for s in series_log])
    save(out / "logs/patch-series.json", series_log)
    if data["build"] == "mas30":
        copy(HERE / "config/BASE-mas30.md", out / "patches/BASE.md")
    else:
        lines = [f"# ModAVD {data['build']} 补丁", "", "按文件名顺序 git am --keep-cr；代码补丁许可证按所改文件的文件头。",
                 "图片和图标的版权与授权限制见仓库 README；不因代码的许可证而获得图片授权。新增补丁的许可证须在发版前逐项复核。", "",
                 "| 目录 | AOSP 仓库 | 基准 | 补丁数 | 原分支头 |", "| --- | --- | --- | --- | --- |"]
        lines += [f"| `{s['dir']}` | `{s['upstream']}` | `{s['base']}` | {s['count']} | `{s['head']}` |" for s in series_log]
        (out / "patches/BASE.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def export_dir(out, s):
    """Where a series of logs/patch-series.json was exported: patches/<dir>, or work/patches-reused/<dir> for one
    that is already published (older logs have no path)."""
    return Path(out) / s.get("path", "patches/" + s["dir"])


def published_patches(data):
    """patches/series.json and BASE.md of the release checkout that step 1 recorded in the snapshot (own.modavd.repo),
    with its path; None without one (mas30, or a direct call)."""
    repo = ((data.get("own") or {}).get("modavd") or {}).get("repo")
    if not repo or data["build"] == "mas30":
        return None
    repo = Path(repo)
    return load(repo / "patches/series.json"), (repo / "patches/BASE.md").read_text(encoding="utf-8-sig"), repo


def leading_same(new_files, old_files):
    """How many patches at the start of two series are the same change (git patch-id), whatever their numbers."""
    n = 0
    for a, b in zip(new_files, old_files):
        if patch_identity(a.read_bytes()) != patch_identity(b.read_bytes()):
            break
        n += 1
    return n


def merge_patch_records(series_log, published, base_text, repo, out, version, number):
    """series.json and BASE.md for the release checkout: the published ones with this release added.

    A series with the repository, base and head of a published one is that series: it keeps the published directory
    and adds nothing. Any other is a new directory: its series.json entry goes first, and BASE.md gets a row in the
    version table, a row in the directory table and a line in the notes under it. What only a person can say (the
    licenses of the new patches, what they do) is left as PLACEHOLDER, which the public metadata check refuses, so
    the release commit cannot go in unfilled. Running it again on its own output adds nothing.
    Returns (series.json, BASE.md, {repo: {"dir", "new"}})."""
    by_key = {(p["repo"], p["base"], p["head"]): p for p in published}
    dirs, new = {}, []
    for s in series_log:
        old = by_key.get((s["repo"], s["base"], s["head"]))
        if old:
            if (old["count"], old["tree"]) != (s["count"], s["tree"]):
                raise ValueError(f"patches/{old['dir']} is published with the base and head of {s['repo']} but another "
                                 f"count or tree; check series.json")
            dirs[s["repo"]] = {"dir": old["dir"], "new": False}
            continue
        if any(p["dir"] == s["dir"] for p in published):
            raise ValueError(f"patches/{s['dir']} is already in series.json with another base or head; a stale copy of "
                             f"an earlier run? Restore patches/series.json and BASE.md from main first")
        dirs[s["repo"]] = {"dir": s["dir"], "new": True}
        new.append({k: v for k, v in s.items() if k not in ("trees", "authors", "path")})
    lines = base_text.replace("\r\n", "\n").split("\n")

    def table(header):
        """Index of the header row and of the first line after the table."""
        starts = [i for i, line in enumerate(lines) if line.startswith(header)]
        if len(starts) != 1 or not lines[starts[0] + 1].startswith("| --- "):
            raise ValueError(f"patches/BASE.md: no single table starting with {header!r}")
        end = starts[0] + 2
        while end < len(lines) and lines[end].startswith("|"):
            end += 1
        return starts[0], end

    def cells(line):
        return [c.strip() for c in line.strip().strip("|").split("|")]

    # version table: | 版本 | qemu | gfxstream | Crashpad |, newest first
    head, end = table("| 版本 |")
    columns = cells(lines[head])[1:]
    by_column = {d["dir"].split("/")[0]: d["dir"] for d in dirs.values()}
    if sorted(c.lower() for c in columns) != sorted(by_column):
        raise ValueError(f"patches/BASE.md: version table columns {columns} differ from the series {sorted(by_column)}")
    want = [by_column[c.lower()] for c in columns]
    # The rows are keyed by the build, not by the version: one build can be published under several versions (a
    # repack, or since 0.41 a release that changes only what is around the emulator, e.g. 0.41.0 on mas-40).
    build_id = f"`Pkg.BuildId=mas-{number}`"
    rows = [i for i in range(head + 2, end) if build_id in cells(lines[i])[0]]
    if len(rows) > 1:
        raise ValueError(f"patches/BASE.md: more than one version row for {build_id}")
    # Only the first export of a build describes it: a rerun, or another version of a published build, adds no notes.
    first = not rows
    if rows:
        row = cells(lines[rows[0]])
        if row[1:] != want:
            raise ValueError(f"patches/BASE.md: the row for {build_id} names {row[1:]}, this build exports {want}")
        # the version as a whole (0.4.0 is not in 0.40.0)
        if not re.search(rf"(?<![0-9.]){re.escape(version)}(?![0-9])", row[0]):
            # another version of a published build: 0.37.0（`Pkg.BuildId=mas-37`；0.37.1 相同）
            row[0] = re.sub(r"）$", f"；{version} 相同）", row[0]) if row[0].endswith("）") else f"{row[0]}（{version} 相同）"
            lines[rows[0]] = "| " + " | ".join(row) + " |"
    else:
        lines.insert(head + 2, f"| {version}（{build_id}） | " + " | ".join(want) + " |")

    # directory table: | 目录 | AOSP 仓库 | 基准 | 补丁数 | 回放后的树 | 许可证 |, grouped by repository, newest first
    head, end = table("| 目录 |")
    for s in reversed(new):
        if any(f"`{s['dir']}`" in line for line in lines[head + 2:end]):
            continue
        group = s["dir"].split("/")[0] + "/"
        at = next((i for i in range(head + 2, end) if cells(lines[i])[0].startswith("`" + group)), end)
        lines.insert(at, f"| `{s['dir']}` | `{s['upstream']}` | `{s['base']}` | {s['count']} | `{s['head']}` | "
                         f"{PLACEHOLDER}：按所改文件的文件头逐个归许可证，写法照下面几行 |")
        end += 1

    # notes: the bullet list right under the directory table, newest first
    head, end = table("| 目录 |")
    at = end
    while at < len(lines) and not lines[at].strip():
        at += 1
    if at >= len(lines) or not lines[at].startswith("- "):
        raise ValueError("patches/BASE.md: no bullet list under the directory table")
    names = dict(zip((c.lower() for c in columns), columns))
    notes = []
    extended = {}   # repo -> number of its first new patch, for a new series that continues the previous one
    for s in series_log:
        d = dirs[s["repo"]]
        if d["new"]:
            if any(line.startswith(f"- `{d['dir']}` 的 ") for line in lines):
                continue
            previous = next((p for p in published if p["repo"] == s["repo"]), None)
            same = 0
            if previous:
                same = leading_same(sorted(export_dir(out, s).glob("*.patch")),
                                    sorted((repo / "patches" / safe_rel(previous["dir"])).glob("*.patch")))
            if same == s["count"]:
                what = f"全部 {s['count']} 个与 `{previous['dir']}` 是同一批提交（补丁文件头里的序号不同）"
            elif same:
                what = (f"0001–{same:04d} 与 `{previous['dir']}` 是同一批提交（补丁文件头里的序号不同），"
                        f"{same + 1:04d}–{s['count']:04d} 是 mas-{number} 新增的：{PLACEHOLDER}（新增的补丁各做了什么）")
                extended[s["repo"]] = same + 1
            else:
                what = f"0001–{s['count']:04d} 都是 mas-{number} 的：{PLACEHOLDER}（和已发布的目录有什么不同）"
            notes.append(f"- `{d['dir']}` 的 {what}。")
        else:
            name = names[d["dir"].split("/")[0]]
            # d["dir"] == s["dir"]: this build's own directory, recorded by an earlier run
            if not first or d["dir"] == s["dir"] or any(line.startswith(f"- {version} 的 {name} 补丁") for line in lines):
                continue
            notes.append(f"- {version} 的 {name} 补丁和已发布的 `{d['dir']}` 是同一批提交（基准、分支头都相同），沿用这个目录；"
                         f"mas-{number} 清单注释里写的也是这个目录。")
    lines[at:at] = notes
    text = "\n".join(lines)
    # The qemu license section names, for each range of patches, the commit whose file headers it was read from, and
    # ends with a replay example of the newest qemu series. Both follow mechanically from a new qemu series that adds
    # patches to the previous one; the license table itself stays for a person (see the directory row's PLACEHOLDER).
    for s in new:
        if not s["dir"].startswith("qemu/") or s["repo"] not in extended:
            continue
        first_new = extended[s["repo"]]
        text = re.sub(r"(这张表是按编译时提交上各文件开头的许可证声明逐个归出来的（[^\n]*?)）。",
                      lambda m: f"{m[1]}，{first_new:04d}–{s['count']:04d} 按 `{s['head'][:8]}`）。", text, count=1)
        replay = [(r"回放示例（以 mas-\d+ 的 qemu 为例）", f"回放示例（以 mas-{number} 的 qemu 为例）"),
                  (r"git checkout -b \S+ [0-9a-f]{40}", f"git checkout -b {s['dir'].split('/')[1]} {s['base']}"),
                  (r"git am --keep-cr \.\./patches/qemu/[^/\s]+/\*\.patch", f"git am --keep-cr ../patches/{s['dir']}/*.patch")]
        if all(len(re.findall(pattern, text)) == 1 for pattern, _ in replay):
            for pattern, value in replay:
                text = re.sub(pattern, lambda m: value, text)
    return new + published, text, dirs


def patch_identity(data):
    """`git patch-id --stable` of one patch file: the diff only, without headers or line numbers."""
    p = subprocess.run(["git", "patch-id", "--stable"], input=data, env=GIT_ENV, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode or not p.stdout.split():
        raise RuntimeError("git patch-id failed: " + p.stderr.decode("utf-8", "replace")[-2000:])
    return p.stdout.split()[0].decode()


def patch_subject(data):
    return re.sub(r"^\[PATCH[^\]]*\]\s*", "", str(checks.patch_mail(data)["Subject"] or "")).strip()


def incoming(args):
    """Every merged incoming/<topic>/*.patch must be in the new series: same patch-id, or (after a rebase moved its
    context) same author and subject. Matched topics are git rm'd from the release repository, into the release
    commit; a topic held for a later release is named with --keep-incoming."""
    out = Path(args.output)
    repo = release_repository(args.repository, getattr(args, "protect", None))
    keep = set(args.keep_incoming or [])
    exported = []
    for s in load(out / "logs/patch-series.json"):
        for p in sorted(export_dir(out, s).glob("*.patch")):
            data = p.read_bytes()
            exported.append((s["dir"] + "/" + p.name, patch_identity(data), checks.patch_author(data), patch_subject(data)))
    log, missing, done = [], [], []
    for topic in sorted(d for d in (repo / "incoming").glob("*") if d.is_dir()):
        if topic.name in keep:
            log.append({"topic": topic.name, "kept": True})
            continue
        patches = sorted(topic.glob("*.patch"))
        if not patches:
            missing.append(topic.name + "/ (no patches)")
        for p in patches:
            data = p.read_bytes()
            ident, author, subject = patch_identity(data), checks.patch_author(data), patch_subject(data)
            hits = [(e[0], "patch-id") for e in exported if e[1] == ident]
            hits = hits or [(e[0], "author+subject") for e in exported if (e[2], e[3]) == (author, subject)]
            if len(hits) != 1:
                missing.append(f"{topic.name}/{p.name}")
            else:
                log.append({"topic": topic.name, "patch": p.name, "in": hits[0][0], "matched_by": hits[0][1]})
        done.append(topic)
    if missing:
        raise ValueError(f"Merged incoming patches not found exactly once in the new series (import them, or pass "
                         f"--keep-incoming <topic>): {missing}")
    for topic in done:
        # With the checkout's own line-ending settings (STATUS_ENV), like changed(): a text file of the topic other
        # than its patches (*.patch -text) is CRLF in a core.autocrlf=true checkout, and under GIT_ENV's
        # core.autocrlf=false git rm takes it for a local modification and refuses (0.38.0). Real local changes still
        # stop it: no -f.
        run("git", "-C", repo, "--no-optional-locks", "rm", "-r", "-q", "--", topic.relative_to(repo).as_posix(),
            env=STATUS_ENV)
    save(out / "logs/incoming.json", log)
    kept = [e["topic"] for e in log if e.get("kept")]
    print(f"Incoming: {len(done)} topics found in the new series and removed from the repository; kept {kept or 'none'}")


def replay(args):
    out = Path(args.output)
    summaries = []
    for s in load(out / "logs/patch-series.json"):
        work = out / "work/replay" / s["dir"].replace("/", "-")
        work.mkdir(parents=True)
        repo = Path(args.emu_src) / s["repo"]
        objects = git(repo, "rev-parse", "--path-format=absolute", "--git-path", "objects")
        git(work, "init", "-q")
        (work / ".git/objects/info/alternates").write_text(Path(objects).as_posix() + "\n", encoding="utf-8", newline="\n")
        git(work, "config", "user.name", "qiyinxi")
        git(work, "config", "user.email", "qiyinxi@foxmail.com")
        git(work, "config", "gc.auto", "0")
        git(work, "config", "maintenance.auto", "false")
        git(work, "checkout", "-q", "--detach", s["base"])
        files = sorted(export_dir(out, s).glob("*.patch"))
        git(work, "am", "--keep-cr", "-q", *files)
        commits = git(work, "rev-list", "--reverse", s["base"] + "..HEAD").splitlines()
        trees = [git(work, "rev-parse", c + "^{tree}") for c in commits]
        authors = git(work, "log", "--reverse", "--format=%an <%ae>", s["base"] + "..HEAD").splitlines()
        # Each author is checked against its own message: an upstream author only on an upstream cherry-pick.
        log = git(work, "log", "--format=%an <%ae>%x00%B%x01", s["base"] + "..HEAD")
        if trees != s["trees"] or authors != s["authors"] or not checks.commits_allowed(log):
            raise ValueError(f"Replayed trees/authors/trailers differ: {s['dir']}")
        summaries.append({"series": s["dir"], "commits": len(commits), "tree_mismatches": 0, "authors": sorted(set(authors))})
        print(f"Replayed {s['dir']}: {len(commits)} commits, every tree equal")
    save(out / "logs/patch-replay.json", summaries)


def sources(args):
    out = Path(args.output)
    data = load(out / "snapshot.json")
    baseline = Path(args.baseline)
    dest = out / "source"
    dest.mkdir()
    old_manifest = ET.parse(MANIFEST_MAS30).getroot()
    new_text = data.get("manifest_xml") or MANIFEST_MAS30.read_text(encoding="utf-8")
    new_manifest = ET.fromstring(new_text)
    old = {p.get("path"): p.get("revision") for p in old_manifest.findall("project")}
    new = {p.get("path"): p.get("revision") for p in new_manifest.findall("project")}
    source_projects = (HERE / "config/source-projects.txt").read_text(encoding="utf-8-sig").splitlines()
    source_projects = [p for p in source_projects if p and not p.startswith("#")]
    # Changes to prebuilts invalidate their source correspondence. Fail closed until the new bundle is reviewed.
    supported = set(source_projects) | {s["repo"] for s in data["series"]}
    unsupported = [p for p in old if old[p] != new.get(p) and p not in supported]
    if unsupported:
        raise ValueError(f"Other manifest projects changed; review prebuilt source attachments first: {unsupported}")
    replaced = {p.replace("/", "-") + "-" + old[p][:12] + ".tar.xz" for p in source_projects if new[p] != old[p]}
    summary = []
    sums = baseline_sums(baseline)
    for name, expected in sums.items():
        if name.endswith(".zip") or name in replaced:
            continue
        if data["build"] != "mas30" and name in {"manifest-mas30.xml", "manifest-aosp-base.xml", "SOURCES.txt"}:
            continue
        copy(baseline / "source" / name, dest / name)
        if sha(dest / name) != expected:
            raise ValueError(f"Reused source hash differs: {name}")
        summary.append({"file": name, "action": "reused", "sha256": expected})
    for project in source_projects:
        if old[project] == new[project]:
            continue
        disk_budget(out, args.min_free_gb)
        rev = new[project]
        name = project.replace("/", "-") + "-" + rev[:12] + ".tar.xz"
        tar = out / "work" / (name[:-3])
        repo = Path(args.emu_src) / project
        git(repo, "archive", "--format=tar", "--prefix=" + project + "/", "-o", str(tar), rev)
        with open(tar, "rb") as src, lzma.open(dest / name, "wb", preset=6) as target:
            shutil.copyfileobj(src, target)
        summary.append({"file": name, "action": "archived", "revision": rev, "sha256": sha(dest / name)})
    if data["build"] != "mas30":
        # this build's own series (patches/series.json is the release checkout's, all versions)
        series = [{k: v for k, v in s.items() if k not in ("trees", "authors", "path")} for s in load(out / "logs/patch-series.json")]
        published_series = load(Path(data["own"]["modavd"]["repo"]) / "patches/series.json")
        (dest / ("manifest-" + data["build"] + ".xml")).write_text(
            annotated_manifest(MANIFEST_MAS30.read_text(encoding="utf-8"), new_manifest, data["build"], series,
                               data["upstream"], published_series),
            encoding="utf-8")
        base = ET.fromstring(new_text)
        for p in base.findall("project"):
            for s in data["series"]:
                if p.get("path") == s["repo"]:
                    p.set("revision", s["base"])
        (dest / "manifest-aosp-base.xml").write_text(
            annotated_manifest((baseline / "source/manifest-aosp-base.xml").read_text(encoding="utf-8-sig"), base,
                               data["build"], series, data["upstream"], published_series),
            encoding="utf-8")
        text = (baseline / "source/SOURCES.txt").read_text(encoding="utf-8-sig")
        # "ModAVD-37.2.10-mas30.zip" is the main ZIP name, which is now ModAVD-<version>-<platform>.zip.
        text = text.replace("ModAVD-37.2.10-mas30", package_name(data["version"])).replace("37.2.10-mas30", data["version"])
        text = text.replace("mas-30", "mas-" + data["build"][3:]).replace("mas30", data["build"])
        for p in source_projects:
            text = text.replace(p.replace("/", "-") + "-" + old[p][:12], p.replace("/", "-") + "-" + new[p][:12])
        summary.append(own_sources(out, dest, data["own"]))
        text = text.rstrip("\n") + "\n" + own_sources_text(data["own"])
        (dest / "SOURCES.txt").write_text(text, encoding="utf-8")
    save(out / "logs/source-assets.json", summary)
    print(f"Source assets: reused {sum(s['action']=='reused' for s in summary)}, archived {sum(s['action']=='archived' for s in summary)}")


def own_sources_text(own):
    """Section 6 of SOURCES.txt: what modavd-src holds and which public version each part is."""
    m, ime, extras = own["manager"], own["ime"], own.get("extras")
    shim = bool(extras) and MUMU_SHIM in extras.get("files", {})
    return OWN_SOURCES_TEXT.format(
        name=OWN_SOURCES_NAME, manager_tag=m["tag"], manager_commit=m["commit"][:12], manager_version=m["version"], go=m["go"],
        modavd_commit=own["modavd"]["commit"][:12],
        ime=OWN_SOURCES_IME_TEXT.format(ime_tag=ime["tag"], ime_commit=ime["commit"][:12]) if ime else "",
        extras=OWN_SOURCES_EXTRAS_TEXT.format(
            extras_tag=extras["tag"], extras_commit=extras["commit"][:12],
            shim="和 mumu-shim\\shell\\sdk\\external_renderer_ipc.dll（MuMu 兼容外壳）" if shim else "",
            shim_asset="、external_renderer_ipc.dll" if shim else "")
        if extras else "")


def own_sources(out, dest, own):
    """OWN_SOURCES_NAME into dest: OWN_SOURCES_ROOT (LICENSE, NOTICE) from this repository's HEAD at the root of
    modavd-src/, then the public repositories as published, each under its own folder (see OWN_SOURCES_NAME).
    Reproducible: git archive stamps every entry with its commit time and fixed owner and modes, the root files get
    the newest of those commit times, the parts go in a fixed order, xz at a fixed preset."""
    repo = REPO
    if changed(repo, *OWN_SOURCES_ROOT.values()):
        raise ValueError("Uncommitted changes under " + ", ".join(OWN_SOURCES_ROOT.values()) + "; the source bundle takes them from HEAD")
    rev = git(repo, "rev-parse", "HEAD")
    parts = [("ModAVDManager", own["manager"]["repo"], own["manager"]["commit"], [])]
    if own["ime"]:
        parts.append(("ModAVDHostIME", own["ime"]["repo"], own["ime"]["commit"], []))
    if own.get("extras"):
        parts.append(("ModAVDExtras", own["extras"]["repo"], own["extras"]["commit"], []))
    parts.append(("ModAVD", own["modavd"]["repo"], own["modavd"]["commit"], ["bridge"]))
    work = out / "work/modavd-src"
    work.mkdir(parents=True, exist_ok=True)
    for folder, path, commit, paths in parts:
        git(path, "archive", "--format=tar", f"--prefix=modavd-src/{folder}/", "-o", str(work / f"{folder}.tar"), commit,
            *(["--", *paths] if paths else []))
    mtime = max(int(git(path, "show", "-s", "--format=%ct", commit)) for _, path, commit, _ in parts)
    tar = work / OWN_SOURCES_NAME[:-3]
    with tarfile.open(tar, "w", format=tarfile.PAX_FORMAT) as target:
        for entry, path in OWN_SOURCES_ROOT.items():
            blob = run("git", "-C", repo, "--no-optional-locks", "cat-file", "blob", f"{rev}:{path}")
            if entry == "LICENSE" and hashlib.sha256(blob).hexdigest() != AGPL_SHA256:
                raise ValueError(f"{path} at {rev[:12]} is not the AGPL-3.0 full text with LF line endings")
            info = tarfile.TarInfo("modavd-src/" + entry)
            info.size, info.mtime, info.mode, info.uname, info.gname = len(blob), mtime, 0o644, "root", "root"
            target.addfile(info, io.BytesIO(blob))
        for folder, *_ in parts:
            with tarfile.open(work / f"{folder}.tar") as source:
                for member in source:
                    target.addfile(member, source.extractfile(member) if member.isfile() else None)
    with open(tar, "rb") as src, lzma.open(dest / OWN_SOURCES_NAME, "wb", preset=6) as target:
        shutil.copyfileobj(src, target)
    return {"file": OWN_SOURCES_NAME, "action": "archived", "revisions": {folder: commit for folder, _, commit, _ in parts},
            "sha256": sha(dest / OWN_SOURCES_NAME)}


def manifest_shape(root):
    return [(e.tag, sorted(e.attrib.items()), [(c.tag, sorted(c.attrib.items())) for c in e]) for e in root]


def annotated_manifest(template, expected, build, series, upstream, published_series):
    """Rewrite a reviewed mas30 manifest so its comments survive; fail closed unless it parses to `expected`."""
    text = template.replace("mas-30", "mas-" + build[3:]).replace("emulator 37.2.10", "emulator " + upstream)
    text, n = re.subn(r"\d{4}-\d{2}-\d{2} 生成", datetime.date.today().isoformat() + " 生成", text)
    if n != 1:
        raise ValueError("Manifest header date not found")
    # Unchanged series keep their published directory, even when the snapshot exports them under this build.
    published = {(s["repo"], s["base"], s["head"]): s["dir"] for s in published_series}
    for s in series:
        directory = published.get((s["repo"], s["base"], s["head"]), s["dir"])
        text, n = re.subn(rf"(<!-- {re.escape(s['repo'])}：AOSP 基准 )[0-9a-f]{{40}}，本地分支 \S+ = 基准 \+ \d+ 个提交，HEAD [0-9a-f]{{40}} -->",
                          lambda m: f"{m[1]}{s['base']}，补丁系列 patches/{directory} = 基准 + {s['count']} 个提交，HEAD {s['head']} -->", text)
        if n != 1:
            raise ValueError(f"Manifest comment for {s['repo']} not found")
    for p in expected.findall("project"):
        text, n = re.subn(rf'(<project [^>]*path="{re.escape(p.get("path"))}"[^>]*revision=")[0-9a-f]{{40}}"',
                          lambda m: f'{m[1]}{p.get("revision")}"', text)
        if n != 1:
            raise ValueError(f"Manifest project {p.get('path')} not found once")
    if manifest_shape(ET.fromstring(text.encode("utf-8"))) != manifest_shape(expected):
        raise ValueError("Annotated manifest differs from the generated one")
    return text


def release_assets(out):
    """Main ZIP (no top-level folder) and source bundle, in table order."""
    version = load(Path(out) / "snapshot.json")["version"]
    return [Path(out) / (package_name(version) + ".zip"), Path(out) / f"ModAVD-{version}-sources.zip"]


def sums(args):
    out = Path(args.output)
    assets = release_assets(out)
    if sorted(out.glob("ModAVD-*.zip")) != sorted(assets):
        raise ValueError("Expected exactly the main and source ZIPs")
    text = "".join(f"{sha(p)}  {p.name}\n" for p in sorted(assets))
    (out / "SHA256SUMS.txt").write_text(text, encoding="utf-8")
    files = [p for p in (out / "source").iterdir() if p.name != "SHA256SUMS.txt"]
    expected = {p.name: sha(p) for p in files}
    if read_sums(out / "source/SHA256SUMS.txt") != expected:
        raise ValueError("Internal source checksums differ")
    save(out / "release-assets.json", {p.name: {"bytes": p.stat().st_size, "sha256": sha(p)} for p in [*assets, *files]})
    print(f"Checksums: {len(assets)} outer assets, {len(files)} inner source files")


def bundle(args):
    out = Path(args.output)
    data = load(out / "snapshot.json")
    name = "ModAVD-" + data["version"] + "-sources"
    source = out / "source"
    files = sorted(source.iterdir())
    (source / "SHA256SUMS.txt").write_text("".join(f"{sha(p)}  {p.name}\n" for p in files), encoding="utf-8")
    stage = out / "work/source-bundle" / name
    stage.mkdir(parents=True)
    for p in source.iterdir():
        copy(p, stage / p.name)
    archive = out / (name + ".zip")
    run(args.sevenzip, "a", "-tzip", "-mx0", "-mcu=on", archive, name, cwd=stage.parent)
    got = {}
    with zipfile.ZipFile(archive) as z:
        for e in z.infolist():
            safe_rel(e.orig_filename)
            if not e.orig_filename.startswith(name + "/") or (any(ord(c)>127 for c in e.orig_filename) and not e.flag_bits & 0x800):
                raise ValueError("Invalid source ZIP path/encoding")
            if e.is_dir():
                continue
            if e.compress_type != zipfile.ZIP_STORED or e.filename in got:
                raise ValueError("Source ZIP must store each file exactly once")
            with z.open(e) as f:
                got[e.filename[len(name)+1:]] = hashlib.file_digest(f, "sha256").hexdigest()
    if got != map_files(source):
        raise ValueError("Source bundle members differ")
    repo = release_repository(args.repository, args.protect)
    for p in source.glob("manifest-*.xml"):
        target = repo / "manifests" / p.name
        if not target.exists() or sha(target) != sha(p):
            copy(p, target)
    print(f"Stored source bundle: {len(files)} source files + internal SHA256SUMS.txt; manifests synchronized")


def render_changes(changes):
    """One `### 分类` block per category, in the fixed category order."""
    blocks = []
    for category in checks.CATEGORIES:
        if category in changes:
            blocks.append(f"### {category}\n\n" + "\n".join("- " + item for item in changes[category]))
    return "\n\n".join(blocks)


def text_fields(version, number, upstream, changes, changelog):
    """The template fields that come from changes.md and changelog.json (everything but the asset rows)."""
    sections = read_changes(changes)
    log = read_changelog(changelog, version, number)
    machine = {"version": "v" + version, "build": number, "upstream": upstream, "min_automas": log[0]["min_automas"],
               "changes": {e["version"]: e["changes"] for e in log[:checks.MACHINE_VERSIONS]}}
    return {"MACHINE": checks.machine_line(machine), "VERSION": version, "BUILD_ID": f"mas-{number}", "UPSTREAM": upstream,
            "CHANGES": render_changes(log[0]["changes"]), "HOW_TO_UPDATE": sections["怎么更新"], "AUTOMAS": sections["AUTO-MAS 版本"],
            "KNOWN": sections["已知问题"], "REGRESSION": sections["发布前回归"]}


def render_notes(fields):
    text = (HERE / "release-notes.template.md").read_text(encoding="utf-8")
    for key, value in fields.items():
        text = text.replace("{{" + key + "}}", value)
    if "{{" in text:
        raise ValueError("Unfilled template variable")
    return text


# A rendered asset row is this long whatever the file: name, size and hash all sit below the separator.
SAMPLE_ASSETS = {"MAIN_ROW": "| `ModAVD-0.0.0-win-x64.zip` | 1 字节 | `" + "0" * 64 + "` |",
                 "BUNDLE_ROW": "| `ModAVD-0.0.0-sources.zip` | 1 字节 | `" + "0" * 64 + "` |", "SOURCE_COUNT": "1", "SOURCE_BYTES": "1"}


def notes_budget(version, number, upstream, changes, changelog):
    """How long the two limited parts of the release notes will be, from the texts alone: the machine-readable first
    line (MAX_MACHINE_CHARS) and the announcement above the separator (MAX_ANNOUNCEMENT_CHARS). Neither depends on the
    assets, so preflight and `notes-budget` tell before packaging, instead of step 7 after it (0.38.0: 9157 and 4532).
    Raises with both numbers when one is over; returns (machine, announcement)."""
    text = render_notes({**text_fields(version, number, upstream, changes, changelog), **SAMPLE_ASSETS})
    lines = text.splitlines()
    machine = len(lines[0])
    upper = len("\n".join(lines[1:lines.index(checks.SEPARATOR)])) if checks.SEPARATOR in lines else 0
    over = []
    if machine > checks.MAX_MACHINE_CHARS:
        over.append(f"the machine-readable line would be {machine} chars, limit {checks.MAX_MACHINE_CHARS} (it carries the "
                    f"changelog.json items of the last {checks.MACHINE_VERSIONS} versions: shorten them)")
    if upper > checks.MAX_ANNOUNCEMENT_CHARS:
        over.append(f"the announcement above the separator would be {upper} chars, limit {checks.MAX_ANNOUNCEMENT_CHARS} "
                    f"(this version's changelog.json items and the 怎么更新 / AUTO-MAS 版本 / 已知问题 sections of {Path(changes).name}: shorten them)")
    if over:
        raise ValueError("Release notes too long: " + "; ".join(over))
    checks.check_announcement(text, "v" + version, number)
    return machine, upper


def describe_budget(machine, upper):
    near = [name for name, n, limit in [("machine line", machine, checks.MAX_MACHINE_CHARS),
                                        ("announcement", upper, checks.MAX_ANNOUNCEMENT_CHARS)] if n > 0.9 * limit]
    return (f"machine line {machine}/{checks.MAX_MACHINE_CHARS}, announcement {upper}/{checks.MAX_ANNOUNCEMENT_CHARS} chars"
            + (f" (WARNING: {' and '.join(near)} over 90% of the limit)" if near else ""))


def notes_budget_command(args):
    """`notes-budget`: the lengths, while changelog.json and changes.md are being written (no SDK or output needed)."""
    version = public_version(args)
    upstream =sdk_upstream(args.sdk) if args.sdk else "99.99.99"
    machine, upper = notes_budget(version, build_number(args.build), upstream, args.changes, args.changelog)
    print(f"Release notes v{version}: {describe_budget(machine, upper)}"
          + ("" if args.sdk else "; upstream version assumed 99.99.99 (pass --sdk for the real one)"))


def notes(args):
    out = Path(args.output)
    data = load(out / "snapshot.json")
    version, number = data["version"], build_number(data["build"])
    main, archive = release_assets(out)
    assets = sorted(p for p in (out / "source").iterdir() if p.name != "SHA256SUMS.txt")
    def row(p):
        return f"| `{p.name}` | {p.stat().st_size:,} 字节 | `{sha(p)}` |"
    fields = {**text_fields(version, number, data["upstream"], args.changes, args.changelog),
              "MAIN_ROW": row(main), "BUNDLE_ROW": row(archive),
              "SOURCE_COUNT": str(len(assets)), "SOURCE_BYTES": f"{sum(p.stat().st_size for p in assets):,}"}
    text = render_notes(fields)
    (out / "release-notes.md").write_text(text, encoding="utf-8")
    checks.check_notes(out / "release-notes.md", out / "SHA256SUMS.txt", {p.name: p.stat().st_size for p in [main, archive]})
    checks.check_announcement(text, "v" + version, number)
    print("Release notes generated; announcement, checksums and asset sizes checked")


def privacy_release(args):
    import privacy
    # The private rules never enter this repository: --private-rules (release.ps1 -PrivateRules), else MODAVD_PRIVATE_RULES.
    if args.private_rules:
        privacy.use_rules(args.private_rules)
    if not privacy.PRIVATE_RULES_LOADED:
        raise ValueError(f"Private privacy rules not loaded (release.ps1 -PrivateRules, PrivateRules in the local "
                         f"configuration, or {privacy.PRIVATE_RULES_ENV}); refusing to scan with the public rules only")
    out = Path(args.output)
    files = []
    for scope in ["release", "patches", "source"]:
        for p in sorted((out / scope).rglob("*")):
            if p.is_file():
                # ZIP already verified against release directory; archive members are scanned here.
                files.append((p, p.relative_to(out / scope).as_posix()))
    for name in ["SHA256SUMS.txt", "release-assets.json", "release-notes.md"]:
        files.append((out / name, name))
    allowlist = REPO / "scripts/privacy-allowlist.json"
    cache = getattr(args, "privacy_cache", None)
    if cache:
        # outside the output (-Force deletes it) and outside the checkouts (it would be committed)
        hit = inside(cache, [out, REPO] + ([args.repository] if getattr(args, "repository", None) else []))
        if hit:
            raise ValueError(f"The privacy cache must live outside the release output and the checkouts: {cache} is in {hit}")
        # the run that uploads (release.ps1 -DraftRelease) scans everything and only refreshes the cache
        report = cached_privacy_scan(privacy, files, allowlist, Path(cache),
                                     write_only=bool(getattr(args, "privacy_cache_write_only", False)))
    else:
        report = privacy.scan_files(files, allowlist)
    save(out / "logs/privacy.json", report)
    from_cache = (f" ({report['cache']['hits']} files from the cache {cache}{', ' + report['cache']['note'] if report['cache']['note'] else ''}; "
                  f"{report['cache']['scanned']} scanned)" if cache else " (no cache)")
    print(f"Privacy: {report['files_scanned']} files/members{from_cache}, {len(report['allowed'])} reviewed exceptions, "
          f"{len(report['failures'])} failures", flush=True)
    if report["failures"]:
        raise ValueError("Privacy failures; see logs/privacy.json")


PRIVACY_CACHE_VERSION = 1
PRIVACY_ARCHIVES = (".tar.xz", ".tar.gz")   # what privacy.scan_files opens and scans member by member


def privacy_fingerprint(privacy):
    """What a cached "clean" verdict depends on besides the file itself: the scanner's code, the rules it ran with
    (public and private), and the Python and Unicode versions (what counts as printable, and so which strings are
    checked, comes from the Unicode database). Any of them changing drops the whole cache. The allowlist is not in it:
    a cached entry keeps the exceptions it relied on, and a hit checks each against the current allowlist (adding
    exceptions never turns a clean file into a failing one; removing one sends the files that used it back to the
    scanner)."""
    import unicodedata
    h = hashlib.sha256(Path(privacy.__file__).read_bytes())
    h.update(json.dumps(privacy.RULE_SPECS, sort_keys=True, ensure_ascii=False).encode("utf-8"))
    h.update(json.dumps([list(sys.version_info[:2]), unicodedata.unidata_version]).encode("utf-8"))
    return h.hexdigest()


PRIVACY_ALLOWED_FIELDS = ("member", "path", "rule", "file_sha256", "string_sha256")


def privacy_entry_ok(entry):
    """An entry as cached_privacy_scan writes it. Anything else is dropped and its file scanned again."""
    return (isinstance(entry, dict) and type(entry.get("files")) is int and entry["files"] >= 1
            and isinstance(entry.get("allowed"), list)
            and all(isinstance(a, dict) and all(isinstance(a.get(k), str) for k in PRIVACY_ALLOWED_FIELDS) for a in entry["allowed"]))


def privacy_key(path, name):
    """The file's SHA-256 and where an exception by path would look, as privacy.scan_files computes it: the name
    without the ModAVD-<version>-<platform>/ folder of the package (so an unchanged file of the last release hits).
    An archive is checked member by member under the member names, so only its content counts."""
    digest = sha(path)
    if name.endswith(PRIVACY_ARCHIVES):
        return digest + "\t!archive"
    logical = name.split("!", 1)[-1]
    if logical.startswith("ModAVD-") and "/" in logical:
        logical = logical.split("/", 1)[1]
    return digest + "\t" + logical


def cached_privacy_scan(privacy, files, allowlist, cache_path, write_only=False):
    """privacy.scan_files(files, allowlist), without scanning again what the cache at `cache_path` (this machine only)
    already found clean: same content at the same logical path (an archive: same content), same scanner and rules,
    and every exception the file needed still in the allowlist. Only clean results are kept; a file with a failure is
    scanned every time. The cache is rewritten with this run's entries only, so it follows the latest release.
    An entry that is not shaped as written here is dropped (its file is scanned again).
    write_only: do not read the cache, scan everything, then write it (the run that uploads: a "clean" entry written by
    hand, or by a bug, must not decide what is published).
    The report is privacy.scan_files's, with the cached files' counts and exceptions put back, plus a "cache" entry."""
    fingerprint = privacy_fingerprint(privacy)
    entries, note = {}, ""
    if write_only:
        note = "not read (upload run), everything scanned"
    elif cache_path.is_file():
        try:
            cached = load(cache_path)
            if not isinstance(cached, dict):
                raise ValueError("not an object")
            if cached.get("version") == PRIVACY_CACHE_VERSION and cached.get("fingerprint") == fingerprint:
                if not isinstance(cached.get("entries"), dict):
                    raise ValueError("entries is not an object")
                entries = {k: v for k, v in cached["entries"].items() if privacy_entry_ok(v)}
                dropped = len(cached["entries"]) - len(entries)
                if dropped:
                    note = f"{dropped} malformed entries dropped"
            else:
                note = "scanner or rules changed, cache dropped"
        except (ValueError, KeyError, TypeError, AttributeError):
            note = "cache unreadable, dropped"
    # read as privacy.scan_files reads it
    exceptions = load(allowlist)["exceptions"]
    by_file = {(e["file_sha256"], e["rule"], e["string_sha256"]) for e in exceptions if "file_sha256" in e}
    by_path = {(e["path"], e["rule"], e["string_sha256"]) for e in exceptions if "path" in e}
    def still_allowed(a):
        return (a["file_sha256"], a["rule"], a["string_sha256"]) in by_file or (a["path"], a["rule"], a["string_sha256"]) in by_path
    report = {"files_scanned": 0, "failures": [], "allowed": []}
    fresh, todo = {}, []
    for path, name in files:
        key = privacy_key(path, name)
        entry = entries.get(key)
        if entry is not None and all(still_allowed(a) for a in entry["allowed"]):
            report["files_scanned"] += entry["files"]
            report["allowed"] += [{"file": name + a["member"], **{k: v for k, v in a.items() if k != "member"}} for a in entry["allowed"]]
            fresh[key] = entry
        else:
            todo.append((path, name, key))
    # plain files in one pass; each archive on its own, so its member count and exceptions stay its own
    batches = [[t for t in todo if not t[1].endswith(PRIVACY_ARCHIVES)]] + [[t] for t in todo if t[1].endswith(PRIVACY_ARCHIVES)]
    for batch in [b for b in batches if b]:
        sub = privacy.scan_files([(p, n) for p, n, _ in batch], allowlist)
        for k in ["failures", "allowed"]:
            report[k] += sub[k]
        report["files_scanned"] += sub["files_scanned"]
        for path, name, key in batch:
            archive = name.endswith(PRIVACY_ARCHIVES)
            mine = (lambda e: e["file"].startswith(name + "!")) if archive else (lambda e: e["file"] == name)
            if any(mine(e) for e in sub["failures"]):
                continue
            fresh[key] = {"files": sub["files_scanned"] if archive else 1,
                          "allowed": [{"member": e["file"][len(name):], **{k: e[k] for k in ("path", "rule", "file_sha256", "string_sha256")}}
                                      for e in sub["allowed"] if mine(e)]}
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    temp = cache_path.with_name(cache_path.name + ".tmp")
    save(temp, {"version": PRIVACY_CACHE_VERSION, "fingerprint": fingerprint, "entries": fresh})
    os.replace(temp, cache_path)
    report["cache"] = {"file": str(cache_path), "hits": len(files) - len(todo), "scanned": len(todo), "note": note}
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["snapshot", "preflight", "own", "check-owned", "assemble", "verify-zip", "patches", "replay", "incoming", "sources", "bundle", "sums", "notes", "notes-budget", "privacy-release"])
    for name in ["build", "sdk", "snapshot", "baseline", "package", "apk", "ime-apk", "output", "manager", "manager-repo", "ime-repo",
                 "extras-dll", "mumu-shim", "extras-repo", "emu-src", "sevenzip", "repository", "private-rules", "privacy-cache"]:
        parser.add_argument("--" + name)
    # release/<X>.<Y> of the ModAVD version: the manager and the extras library may be at the head of this branch
    # before their tags exist
    parser.add_argument("--release-branch")
    # the public version X.Y.Z (release.ps1 -Version); without it 0.<N>.<--patch> from --build, as up to 0.40
    parser.add_argument("--version")
    # preflight: every component must be tagged (release.ps1 -DraftRelease)
    parser.add_argument("--require-tags", action="store_true")
    parser.add_argument("--keep-incoming", action="append", default=[])
    # privacy-release with --privacy-cache: scan everything and only write the cache (release.ps1 -DraftRelease)
    parser.add_argument("--privacy-cache-write-only", action="store_true")
    # Directories the release checkout must not be in (source trees, backups, production); repeatable.
    parser.add_argument("--protect", action="append", default=[])
    parser.add_argument("--min-free-gb", type=int, default=120)
    parser.add_argument("--patch", type=int, default=0)
    parser.add_argument("--changes", default=str(HERE / "config/changes.md"))
    parser.add_argument("--changelog", default=str(HERE / "config/changelog.json"))
    args = parser.parse_args()
    # a real package always carries the ModAVDExtras files (see pinned_components); only unit tests build without
    args.require_extras = True
    commands = {"snapshot": snapshot_data, "preflight": preflight, "own": lambda a: owned(a, True), "check-owned": owned,
                "assemble": assemble, "verify-zip": verify_zip, "patches": export_patches, "replay": replay, "incoming": incoming,
                "sources": sources, "bundle": bundle, "sums": sums, "notes": notes, "notes-budget": notes_budget_command,
                "privacy-release": privacy_release}
    try:
        commands[args.command](args)
    except Exception as e:
        print(f"ERROR [{args.command}]: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
