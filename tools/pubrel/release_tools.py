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
# Public version 0.<N>.<P>. mas30 and mas32 were published as 37.2.10-masN, which counts as 0.N.0,
# so builds before this one can only be repackaged (P >= 1), e.g. mas32 -> 0.32.1.
FIRST_NEW_BUILD = 33
MANIFEST = "modavd-manifest.json"
# Binary packages carry the platform (a Linux build may follow); the source bundle does not.
PLATFORM = "win-x64"
# The guest half of the host input method channel (mas-35+), shipped as components/<this name>; the name never
# changes, MAS finds it by name. It goes in only when release.ps1 gets -ImeApk.
IME_APK = "modavd-ime.apk"
# Every APK under components/ pinned by SHA-256 (see component_apks).
COMPONENT_SUMS = HERE / "config/components.SHA256SUMS.txt"
# What shipped after the P3 review of the NOTICE and 说明.txt (see extend_texts): files added to entries already
# there, the entries appended to Part 2 (NOTICE.txt / NOTICE.csv), and a section for 说明.txt.
NOTICE_MORE_FILES = {
    "ModAVD program icon (fan art)": ["(package root) ModAVD.exe"],
    "ModAVD scripts and documents": ["(package root) tools/modavd-bridge.ps1", "(package root) tools/report-config.ps1",
                                     "(package root) 复制配置信息.bat"],
}
NOTICE_ADDITIONS = HERE / "config/notice-additions.txt"
NOTICE_ADDITIONS_CSV = HERE / "config/notice-additions.csv"
README_TOOLS = HERE / "config/readme-tools.txt"
# The Go programs come from the ModAVDManager release ZIP (--manager), checked against release/<version>.SHA256SUMS.txt
# at tag v<version> of a ModAVDManager checkout (--manager-repo): path in the ZIP -> package path.
MODAVD_FILES = {"tools/modavd.exe": "tools/modavd.exe", "ModAVD.exe": "ModAVD.exe"}
MANAGER_ZIP = re.compile(r"ModAVDManager-(\d+\.\d+\.\d+)-win-x64\.zip")
# The bundled scripts, read from package/ of the ModAVD release checkout (--repository) at its HEAD, where package/**
# is stored byte for byte (-text): these go to tools/, the batch files to the package root.
PACKAGE_TOOLS = ["collect-info.ps1", "fetch-components.ps1", "modavd-bridge.ps1", "report-config.ps1"]
PACKAGE_ROOT = ["补全组件.bat", "收集问题信息.bat", "复制配置信息.bat"]
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
{ime}     ModAVD/bridge/         sdk\\emulator\\resources\\modavd 里的 arm64 执行桥：ModAVD 仓库提交 {modavd_commit} 的
                            bridge/。libmodavd_bridge.so（nbshim/jni）、libmodavd_gate.so（companion/jni）用
                            Android NDK r27c 的 ndk-build 按 jni 下的 Android.mk / Application.mk 编（bridge/scripts）；
                            modavd_bridge.rc 是原文件。https://github.com/ModAVD/ModAVD/tree/main/bridge
   包里的脚本和批处理文件（tools\\*.ps1、包根的 .bat）本身就是源码，随主包原样发出，不在这个归档里；
   取自同一提交的 package/：https://github.com/ModAVD/ModAVD/tree/main/package。
"""
OWN_SOURCES_IME_TEXT = """\
     ModAVDHostIME/         components\\modavd-ime.apk（宿主输入法 org.modavd.ime）：ModAVDHostIME {ime_tag}
                            （提交 {ime_commit}）。用 Android SDK build-tools 36.1.0 的 aapt2 / d8 / zipalign /
                            apksigner 和 platforms/android-36 的 android.jar 编（javac --release 17，
                            d8 --min-api 30）；签名用 ModAVD 的发布密钥，不在源码包里。
                            https://github.com/ModAVD/ModAVDHostIME
"""
# changes.md: these go above the github-only separator (in this order), the last one below it.
UPPER_SECTIONS = ["怎么更新", "AUTO-MAS 版本", "已知问题"]
LOWER_SECTIONS = ["发布前回归"]
PLACEHOLDER = "发布前填写"
GIT_ENV = dict(os.environ, GIT_OPTIONAL_LOCKS="0", GIT_CONFIG_COUNT="2",
               GIT_CONFIG_KEY_0="core.autocrlf", GIT_CONFIG_VALUE_0="false",
               GIT_CONFIG_KEY_1="core.longpaths", GIT_CONFIG_VALUE_1="true")


def run(*args, cwd=None):
    p = subprocess.run([str(a) for a in args], cwd=cwd, env=GIT_ENV,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode:
        raise RuntimeError(f"{args[0]} failed ({p.returncode}): {p.stderr.decode('utf-8', 'replace')[-6000:]}")
    return p.stdout


def git(repo, *args):
    return run("git", "-C", repo, "--no-optional-locks", *args).decode("utf-8").strip()


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
    """Entries newest first, starting at the release being built; validated with the public checks."""
    entries = load(path)
    if not isinstance(entries, list):
        raise ValueError("changelog.json must be a list")
    seen = []
    for e in entries:
        if not isinstance(e, dict) or set(e) != {"version", "build", "min_automas", "changes"}:
            raise ValueError(f"changelog entry needs exactly version/build/min_automas/changes: {e}")
        key = checks.version_key(e["version"])
        if key[1] != e["build"]:
            raise ValueError(f"changelog build differs from version: {e['version']}")
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
        if git(repo, "status", "--porcelain", "--untracked-files=no"):
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


def component_apks(args, pinned_path=COMPONENT_SUMS):
    """{package path: source} of the APKs that go into components/, checked against config/components.SHA256SUMS.txt.

    Every APK the package carries is pinned there, and every pinned APK must come with exactly that content: once a
    file has shipped, a release without it would make the incremental update delete it on users' machines. An APK
    that is not pinned is refused, so shipping a new one (or a new build of one) is a reviewed change to that file.
    """
    pinned = read_sums(pinned_path)
    # NTFS ignores case: two names that differ only in case would be one file in the package
    if any(not re.fullmatch(r"components/[^/]+", rel) for rel in pinned) or len({r.casefold() for r in pinned}) != len(pinned):
        raise ValueError(f"{Path(pinned_path).name}: every line must be a distinct components/<file name> (case-insensitive)")
    given = {"components/" + Path(args.apk).name: Path(args.apk)}
    if getattr(args, "ime_apk", None):
        given["components/" + IME_APK] = Path(args.ime_apk)
    if len({r.casefold() for r in given}) != 1 + bool(getattr(args, "ime_apk", None)):
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
    return given


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
    version = release_version(args.build, args.patch)
    number = build_number(args.build)
    sdk = Path(args.sdk) / "emulator"
    props = (sdk / "source.properties").read_text(encoding="utf-8-sig")
    if not re.search(rf"(?m)^Pkg.BuildId=mas-{number}\s*$", props):
        raise ValueError("SDK Pkg.BuildId differs from requested release")
    upstream = sdk_upstream(args.sdk)
    read_changes(args.changes)
    read_changelog(args.changelog, version, number)
    baseline = Path(args.baseline)
    sums = baseline_sums(baseline)
    for name, expected in sums.items():
        if name.endswith(".zip"):
            continue
        src = baseline / "source" / name
        if sha(src) != expected:
            raise ValueError(f"Baseline asset hash differs: {name}")
    for name in ["说明.txt", "LICENSES/NOTICE.txt", "LICENSES/NOTICE.csv"]:
        if not (Path(args.package) / name).is_file():
            raise FileNotFoundError(name)
    notice = (Path(args.package) / "LICENSES/NOTICE.txt").read_text(encoding="utf-8-sig")
    check_agpl(args.package)
    components = component_apks(args)
    for rel in components:
        if f"(package root) {rel}" not in notice:
            raise ValueError(f"LICENSES/NOTICE.txt has no entry for {rel}")
    # modavd-src takes LICENSE and NOTICE from HEAD
    if git(REPO, "status", "--porcelain", "--untracked-files=all", "--", *OWN_SOURCES_ROOT.values()):
        raise ValueError("Uncommitted changes under " + ", ".join(OWN_SOURCES_ROOT.values()) + "; commit first")
    own = public_sources(args)
    print(f"Preflight OK: v{version} (mas-{number}, Emulator {upstream}), changes, changelog, snapshot, baseline hashes, "
          f"AGPL-3.0 full text, pinned components ({', '.join(sorted(Path(r).name for r in components))}), disk budget, output path; "
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


def manager_release(zip_path, repo):
    """The ModAVDManager release ZIP (ModAVDManager-<version>-win-x64.zip from its GitHub Release): the ZIP and the
    programs in it are the ones release/<version>.SHA256SUMS.txt lists at tag v<version> of the checkout `repo`."""
    path = Path(zip_path or "")
    m = MANAGER_ZIP.fullmatch(path.name)
    if not m:
        raise ValueError(f"--manager must be ModAVDManager-<version>-win-x64.zip from its GitHub Release, got {zip_path!r}")
    if not path.is_file():
        raise FileNotFoundError(path)
    version, tag = m[1], "v" + m[1]
    commit = tagged(repo, tag)
    try:
        listed = parse_sums(git(repo, "show", f"{tag}:release/{version}.SHA256SUMS.txt"), f"{tag}:release/{version}.SHA256SUMS.txt")
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
    return {"repo": str(Path(repo).resolve()), "version": version, "tag": tag, "commit": commit,
            "go": go_toolchain(git(repo, "show", f"{tag}:go.mod"))}


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


def public_sources(args):
    """What the package takes from the public repositories, checked: the manager ZIP against the checksums at its
    tag, the IME APK against the tag that lists it, and package/ and bridge/ of the ModAVD release checkout unchanged
    since its HEAD. assemble records the result in the snapshot, so every later step uses the same revisions."""
    own = {"manager": manager_release(args.manager, args.manager_repo),
           "ime": ime_release(args.ime_apk, args.ime_repo) if getattr(args, "ime_apk", None) else None}
    repo = release_repository(args.repository, getattr(args, "protect", None))
    if git(repo, "status", "--porcelain", "--untracked-files=all", "--", "package", "bridge"):
        raise ValueError(f"{repo}: uncommitted changes under package/ or bridge/; the package takes both from HEAD")
    commit = git(repo, "rev-parse", "HEAD")
    for rel in [*("package/" + n for n in PACKAGE_TOOLS + PACKAGE_ROOT), "bridge/nbshim/modavd_bridge.rc"]:
        try:
            git(repo, "cat-file", "-e", f"{commit}:{rel}")
        except RuntimeError:
            raise ValueError(f"{repo}: no {rel} at HEAD {commit[:12]}") from None
    own["modavd"] = {"repo": str(repo), "commit": commit}
    return own


def describe_sources(own):
    ime = f", ModAVDHostIME {own['ime']['tag']}" if own["ime"] else ""
    return f"ModAVDManager {own['manager']['tag']} (Go {own['manager']['go']}){ime}, ModAVD package/ and bridge/ at {own['modavd']['commit'][:12]}"


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
    version = release_version(args.build, args.patch)
    number = build_number(args.build)
    current = read_changelog(args.changelog, version, number)[0]
    own = public_sources(args)
    # Later steps take the public version and the public sources from the output copy of the snapshot.
    data.update(version=version, patch=int(args.patch or 0), upstream=sdk_upstream(args.sdk), min_automas=current["min_automas"],
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
    pinned = read_sums(COMPONENT_SUMS)
    for rel, src in component_apks(args).items():
        copy(src, dest / rel)
        # checked again after the copy: the source may have been rebuilt in between
        if sha(dest / rel) != pinned[rel]:
            raise ValueError(f"{rel} in the package differs from its pinned SHA-256")
    for p in (Path(args.package) / "LICENSES").iterdir():
        if p.is_file():
            copy(p, dest / "LICENSES" / p.name)
    copy(Path(args.package) / "说明.txt", dest / "说明.txt")
    # The reviewed P3 texts say "37.2.10-mas30": the folder name becomes ModAVD-<version>-<platform>, the product
    # name the new version; the build ID and manifest names stay masN.
    for text in [dest / "说明.txt", *list((dest / "LICENSES").glob("NOTICE.*"))]:
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


def copy_public_files(own, manager, dest):
    """Into the package tree `dest`: the bundled scripts, byte for byte from package/ at the recorded ModAVD commit,
    and the programs from the manager ZIP (both checked by public_sources)."""
    scripts = {**{"tools/" + n: n for n in PACKAGE_TOOLS}, **{n: n for n in PACKAGE_ROOT}}
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
    """Bring the P3-reviewed NOTICE.txt / NOTICE.csv and 说明.txt up to what this package ships: NOTICE_MORE_FILES
    into entries already there, config/notice-additions.* at the end of Part 2 (`go`: the Go version the manager was
    built with), config/readme-tools.txt before the troubleshooting section of 说明.txt. Every anchor has to be there
    exactly once."""
    path = dest / "LICENSES/NOTICE.txt"
    text = path.read_text(encoding="utf-8-sig")
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
    for lib, files in NOTICE_MORE_FILES.items():
        hits = [r for r in rows[1:] if r[name_col] == lib]
        if len(hits) != 1:
            raise ValueError(f"NOTICE.csv: entry {lib!r} not found exactly once")
        hits[0][files_col] += " " + " ".join(files)
    added = list(csv.reader(io.StringIO(NOTICE_ADDITIONS_CSV.read_text(encoding="utf-8").replace("{{GO_VERSION}}", go))))
    if added[0] != rows[0]:
        raise ValueError("config/notice-additions.csv: header differs from NOTICE.csv")
    path.write_text(render_csv(rows + added[1:]), encoding="utf-8")

    path = dest / "说明.txt"
    text = path.read_text(encoding="utf-8-sig")
    anchor = "====================\n出问题怎么办\n"
    if text.count(anchor) != 1:
        raise ValueError("说明.txt: the troubleshooting heading not found exactly once")
    path.write_text(text.replace(anchor, README_TOOLS.read_text(encoding="utf-8") + anchor), encoding="utf-8")


def check_notice_coverage(dest):
    """Every file at the package root and below (not sdk/) is named in NOTICE.txt as "(package root) <path>", and so
    is every file of ours under sdk/emulator/resources/modavd/ (as "./<path>"); the license texts excepted. Runs
    before the manifest exists."""
    notice = (dest / "LICENSES/NOTICE.txt").read_text(encoding="utf-8-sig")
    named = set(re.findall(r"^\(package root\) (.+)$", notice, re.M))
    named_sdk = set(re.findall(r"^\./(.+)$", notice, re.M))
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
            # Outside contributors keep their GitHub noreply authorship; any other identity in the local
            # branches (other addresses, worker accounts) is published as IDENTITY.
            original = git(repo, "log", "-1", "--format=%an <%ae>", commit)
            keep = bool(checks.NOREPLY_AUTHOR.fullmatch(original))
            if keep and not checks.am_keeps_name(original):
                raise ValueError(f"{commit}: git am would drop the author name; amend it to 3-60 bytes without @")
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
                     trees=[git(repo, "rev-parse", c + "^{tree}") for c in originals], authors=authors)
        series_log.append(entry)
        print(f"Exported {series['dir']}: {len(files)} patches")
    save(out / "patches/series.json", [{k: v for k, v in s.items() if k not in ("trees", "authors")} for s in series_log])
    save(out / "logs/patch-series.json", series_log)
    if data["build"] == "mas30":
        copy(HERE / "config/BASE-mas30.md", out / "patches/BASE.md")
    else:
        lines = [f"# ModAVD {data['build']} 补丁", "", "按文件名顺序 git am --keep-cr；代码补丁许可证按所改文件的文件头。",
                 "图片和图标的版权与授权限制见仓库 README；不因代码的许可证而获得图片授权。新增补丁的许可证须在发版前逐项复核。", "",
                 "| 目录 | AOSP 仓库 | 基准 | 补丁数 | 原分支头 |", "| --- | --- | --- | --- | --- |"]
        lines += [f"| `{s['dir']}` | `{s['upstream']}` | `{s['base']}` | {s['count']} | `{s['head']}` |" for s in series_log]
        (out / "patches/BASE.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


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
        for p in sorted((out / "patches" / s["dir"]).glob("*.patch")):
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
        git(repo, "rm", "-r", "-q", "--", topic.relative_to(repo).as_posix())
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
        files = sorted((out / "patches" / s["dir"]).glob("*.patch"))
        git(work, "am", "--keep-cr", "-q", *files)
        commits = git(work, "rev-list", "--reverse", s["base"] + "..HEAD").splitlines()
        trees = [git(work, "rev-parse", c + "^{tree}") for c in commits]
        authors = git(work, "log", "--reverse", "--format=%an <%ae>", s["base"] + "..HEAD").splitlines()
        messages = git(work, "log", "--format=%B", s["base"] + "..HEAD")
        if (trees != s["trees"] or authors != s["authors"] or not all(checks.author_allowed(a) for a in authors)
                or re.search(r"(?i)co-authored-by:", messages)):
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
        series = load(out / "patches/series.json")
        (dest / ("manifest-" + data["build"] + ".xml")).write_text(
            annotated_manifest(MANIFEST_MAS30.read_text(encoding="utf-8"), new_manifest, data["build"], series, data["upstream"]),
            encoding="utf-8")
        base = ET.fromstring(new_text)
        for p in base.findall("project"):
            for s in data["series"]:
                if p.get("path") == s["repo"]:
                    p.set("revision", s["base"])
        (dest / "manifest-aosp-base.xml").write_text(
            annotated_manifest((baseline / "source/manifest-aosp-base.xml").read_text(encoding="utf-8-sig"), base, data["build"], series, data["upstream"]),
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
    m, ime = own["manager"], own["ime"]
    return OWN_SOURCES_TEXT.format(
        name=OWN_SOURCES_NAME, manager_tag=m["tag"], manager_commit=m["commit"][:12], manager_version=m["version"], go=m["go"],
        modavd_commit=own["modavd"]["commit"][:12],
        ime=OWN_SOURCES_IME_TEXT.format(ime_tag=ime["tag"], ime_commit=ime["commit"][:12]) if ime else "")


def own_sources(out, dest, own):
    """OWN_SOURCES_NAME into dest: OWN_SOURCES_ROOT (LICENSE, NOTICE) from this repository's HEAD at the root of
    modavd-src/, then the public repositories as published, each under its own folder (see OWN_SOURCES_NAME).
    Reproducible: git archive stamps every entry with its commit time and fixed owner and modes, the root files get
    the newest of those commit times, the parts go in a fixed order, xz at a fixed preset."""
    repo = REPO
    if git(repo, "status", "--porcelain", "--untracked-files=all", "--", *OWN_SOURCES_ROOT.values()):
        raise ValueError("Uncommitted changes under " + ", ".join(OWN_SOURCES_ROOT.values()) + "; the source bundle takes them from HEAD")
    rev = git(repo, "rev-parse", "HEAD")
    parts = [("ModAVDManager", own["manager"]["repo"], own["manager"]["commit"], [])]
    if own["ime"]:
        parts.append(("ModAVDHostIME", own["ime"]["repo"], own["ime"]["commit"], []))
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


def annotated_manifest(template, expected, build, series, upstream):
    """Rewrite a reviewed mas30 manifest so its comments survive; fail closed unless it parses to `expected`."""
    text = template.replace("mas-30", "mas-" + build[3:]).replace("emulator 37.2.10", "emulator " + upstream)
    text, n = re.subn(r"\d{4}-\d{2}-\d{2} 生成", datetime.date.today().isoformat() + " 生成", text)
    if n != 1:
        raise ValueError("Manifest header date not found")
    for s in series:
        text, n = re.subn(rf"(<!-- {re.escape(s['repo'])}：AOSP 基准 )[0-9a-f]{{40}}，本地分支 \S+ = 基准 \+ \d+ 个提交，HEAD [0-9a-f]{{40}} -->",
                          lambda m: f"{m[1]}{s['base']}，补丁系列 patches/{s['dir']} = 基准 + {s['count']} 个提交，HEAD {s['head']} -->", text)
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


def notes(args):
    out = Path(args.output)
    data = load(out / "snapshot.json")
    version, number = data["version"], build_number(data["build"])
    sections = read_changes(args.changes)
    log = read_changelog(args.changelog, version, number)
    machine = {"version": "v" + version, "build": number, "upstream": data["upstream"], "min_automas": log[0]["min_automas"],
               "changes": {e["version"]: e["changes"] for e in log[:checks.MACHINE_VERSIONS]}}
    main, archive = release_assets(out)
    assets = sorted(p for p in (out / "source").iterdir() if p.name != "SHA256SUMS.txt")
    def row(p):
        return f"| `{p.name}` | {p.stat().st_size:,} 字节 | `{sha(p)}` |"
    fields = {"MACHINE": checks.machine_line(machine), "VERSION": version, "BUILD_ID": f"mas-{number}", "UPSTREAM": data["upstream"],
              "CHANGES": render_changes(log[0]["changes"]), "HOW_TO_UPDATE": sections["怎么更新"], "AUTOMAS": sections["AUTO-MAS 版本"],
              "KNOWN": sections["已知问题"], "REGRESSION": sections["发布前回归"],
              "MAIN_ROW": row(main), "BUNDLE_ROW": row(archive),
              "SOURCE_COUNT": str(len(assets)), "SOURCE_BYTES": f"{sum(p.stat().st_size for p in assets):,}"}
    text = (HERE / "release-notes.template.md").read_text(encoding="utf-8")
    for key, value in fields.items():
        text = text.replace("{{" + key + "}}", value)
    if "{{" in text:
        raise ValueError("Unfilled template variable")
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
    report = privacy.scan_files(files, REPO / "scripts/privacy-allowlist.json")
    save(out / "logs/privacy.json", report)
    print(f"Privacy: {report['files_scanned']} files/members, {len(report['allowed'])} reviewed exceptions, {len(report['failures'])} failures", flush=True)
    if report["failures"]:
        raise ValueError("Privacy failures; see logs/privacy.json")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["snapshot", "preflight", "own", "check-owned", "assemble", "verify-zip", "patches", "replay", "incoming", "sources", "bundle", "sums", "notes", "privacy-release"])
    for name in ["build", "sdk", "snapshot", "baseline", "package", "apk", "ime-apk", "output", "manager", "manager-repo", "ime-repo",
                 "emu-src", "sevenzip", "repository", "private-rules"]:
        parser.add_argument("--" + name)
    parser.add_argument("--keep-incoming", action="append", default=[])
    # Directories the release checkout must not be in (source trees, backups, production); repeatable.
    parser.add_argument("--protect", action="append", default=[])
    parser.add_argument("--min-free-gb", type=int, default=120)
    parser.add_argument("--patch", type=int, default=0)
    parser.add_argument("--changes", default=str(HERE / "config/changes.md"))
    parser.add_argument("--changelog", default=str(HERE / "config/changelog.json"))
    args = parser.parse_args()
    commands = {"snapshot": snapshot_data, "preflight": preflight, "own": lambda a: owned(a, True), "check-owned": owned,
                "assemble": assemble, "verify-zip": verify_zip, "patches": export_patches, "replay": replay, "incoming": incoming,
                "sources": sources, "bundle": bundle, "sums": sums, "notes": notes, "privacy-release": privacy_release}
    try:
        commands[args.command](args)
    except Exception as e:
        print(f"ERROR [{args.command}]: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
