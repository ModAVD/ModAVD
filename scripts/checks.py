"""Public repository checks: patch replay, release metadata, byte preservation."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

# Release notes from 0.32.1 on: a one-line machine-readable comment first, then the announcement that is
# synced to MirrorChyan and shown by AUTO-MAS, then this separator, then GitHub-only details.
MACHINE_PREFIX = "<!-- modavd: "
SEPARATOR = "<!-- modavd:github-only -->"
CATEGORIES = ("新增", "变更", "修复", "移除", "说明")
MACHINE_VERSIONS = 10
MAX_ANNOUNCEMENT_CHARS = 4000
MAX_MACHINE_CHARS = 8000
MANIFEST = "modavd-manifest.json"


def version_key(value):
    """v0.<N>.<P>: stage (0 while in testing), build N (Pkg.BuildId mas-N), repack count P."""
    m = re.fullmatch(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", value if isinstance(value, str) else "")
    if not m:
        raise ValueError(f"Version must look like v0.33.0: {value!r}")
    return tuple(int(x) for x in m.groups())


def build_of(value):
    """The build number N of v0.<N>.<P>."""
    return version_key(value)[1]


def check_min_automas(value):
    if value is not None and not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-(?:alpha|beta)\.[0-9]+)?", value if isinstance(value, str) else ""):
        raise ValueError(f"min_automas must be null or like 5.7.0 / 5.7.0-beta.5: {value!r}")


def check_changes(changes):
    """{"v0.33.0": {"修复": ["…"]}}: known categories, one-line items that cannot end an HTML comment."""
    if not isinstance(changes, dict) or not changes:
        raise ValueError("changes must be a non-empty object")
    for version, groups in changes.items():
        version_key(version)
        if not isinstance(groups, dict) or not groups or any(c not in CATEGORIES for c in groups):
            raise ValueError(f"{version}: categories must be among {CATEGORIES}")
        for items in groups.values():
            if not isinstance(items, list) or not items:
                raise ValueError(f"{version}: each category needs at least one item")
            for item in items:
                if not isinstance(item, str) or not item.strip() or "\n" in item or "\r" in item or "--" in item:
                    raise ValueError(f"{version}: items must be non-empty single lines without '--': {item!r}")


def machine_line(data):
    text = json.dumps(data, ensure_ascii=False, separators=(",", ":"))
    if "--" in text:
        raise ValueError("Machine-readable line must not contain '--'")
    return MACHINE_PREFIX + text + " -->"


def check_announcement(text, version=None, build=None):
    """Machine line, separator and announcement size of 0.32.1+ release notes."""
    lines = text.splitlines()
    m = re.fullmatch(re.escape(MACHINE_PREFIX) + r"(\{.*\}) -->", lines[0] if lines else "")
    if not m or len(lines[0]) > MAX_MACHINE_CHARS or "--" in m[1]:
        raise ValueError("First line must be a single machine-readable <!-- modavd: {...} --> comment")
    data = json.loads(m[1])
    if set(data) != {"version", "build", "upstream", "min_automas", "changes"}:
        raise ValueError("Machine line needs exactly version/build/upstream/min_automas/changes")
    key = version_key(data["version"])
    if data["build"] != key[1] or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", str(data["upstream"])):
        raise ValueError("Machine line build/upstream invalid")
    check_min_automas(data["min_automas"])
    check_changes(data["changes"])
    keys = [version_key(v) for v in data["changes"]]
    if keys[0] != key or keys != sorted(set(keys), reverse=True) or len(keys) > MACHINE_VERSIONS:
        raise ValueError(f"changes must start with this version, newest first, at most {MACHINE_VERSIONS} versions")
    if (version is not None and data["version"] != version) or (build is not None and data["build"] != build):
        raise ValueError("Machine line version/build differ from this release")
    if lines.count(SEPARATOR) != 1:
        raise ValueError(f"Separator line {SEPARATOR} must appear exactly once")
    upper = "\n".join(lines[1:lines.index(SEPARATOR)])
    if len(upper) > MAX_ANNOUNCEMENT_CHARS:
        raise ValueError(f"Announcement above the separator is {len(upper)} chars; keep it within {MAX_ANNOUNCEMENT_CHARS}")
    if re.search(r"[0-9a-f]{64}", upper):
        raise ValueError("Checksums belong below the separator")
    return data


def check_manifest(tree):
    """modavd-manifest.json lists every other file of the package with its size and SHA-256."""
    tree = Path(tree)
    data = json.loads((tree / MANIFEST).read_text(encoding="utf-8"))
    if data.get("schema") != 1 or set(data) != {"schema", "version", "build", "upstream", "platform", "min_automas", "files"}:
        raise ValueError("Manifest fields differ from schema 1")
    if build_of(data["version"]) != data["build"]:
        raise ValueError("Manifest build differs from version")
    if not re.fullmatch(r"(?:win|linux)-(?:x64|arm64)", str(data["platform"])):
        raise ValueError(f"Manifest platform invalid: {data['platform']!r}")
    check_min_automas(data["min_automas"])
    actual = {}
    for p in tree.rglob("*"):
        rel = p.relative_to(tree).as_posix()
        if p.is_file() and rel != MANIFEST:
            with open(p, "rb") as f:
                actual[rel] = {"size": p.stat().st_size, "sha256": hashlib.file_digest(f, "sha256").hexdigest()}
    if data["files"] != actual:
        raise ValueError("Manifest file list differs from the package")
    return data


def git(repo, *args):
    result = subprocess.run(["git", "-c", "core.autocrlf=false", "-c", "core.longpaths=true", "-C", str(repo), *map(str, args)],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"git {' '.join(map(str, args[:4]))}: {result.stderr[-6000:]}")
    return result.stdout.strip()


def check_notes(notes, sums, sizes=None, source_sums=None):
    text = Path(notes).read_text(encoding="utf-8-sig")
    known = re.search(r"(?ms)^## 已知问题\s*\n(.*?)(?=^## |\Z)", text)
    if known:
        for line in known[1].splitlines():
            if line.startswith("- ") and not re.search(r"https://github\.com/ModAVD/ModAVD/issues/[1-9][0-9]*", line):
                raise ValueError("Each known issue must link to a repository issue")
    hashes = {}
    checksum_lines = Path(sums).read_text(encoding="utf-8-sig").splitlines()
    if source_sums:
        checksum_lines += Path(source_sums).read_text(encoding="utf-8-sig").splitlines()
    for line in checksum_lines:
        m = re.fullmatch(r"([0-9a-f]{64})  ([^/\\]+)", line)
        if not m or m[2] in hashes:
            raise ValueError(f"Invalid/duplicate checksum line: {line}")
        hashes[m[2]] = m[1]
    rows = {}
    for line in text.splitlines():
        m = re.match(r"\| `([^`]+)` \| (.+?) \| `([0-9a-f]{64})` \|", line)
        if not m:
            continue
        if m[1] in rows:
            raise ValueError(f"Duplicate notes asset: {m[1]}")
        size = re.search(r"([0-9][0-9,]*) 字节", m[2])
        if not size or int(size[1].replace(",", "")) <= 0:
            raise ValueError(f"Missing positive byte size: {m[1]}")
        n = int(size[1].replace(",", ""))
        human = re.match(r"([0-9]+\.[0-9]) (KiB|MiB)", m[2])
        if human:
            unit = 1024 if human[2] == "KiB" else 1024**2
            if f"{n/unit:.1f}" != human[1]:
                raise ValueError(f"Human-readable size disagrees with bytes: {m[1]}")
        if sizes is not None and sizes.get(m[1]) != n:
            raise ValueError(f"Actual size differs: {m[1]}")
        rows[m[1]] = m[3]
    if not hashes or rows != hashes:
        raise ValueError("Release notes asset table differs from SHA256SUMS.txt")
    print(f"Release metadata OK: {Path(notes).name}, {len(rows)} assets")


def metadata(root):
    attrs = (root / ".gitattributes").read_text(encoding="utf-8-sig")
    if not re.search(r"(?m)^\*\.patch\s+-text\s*$", attrs):
        raise ValueError(".gitattributes must include *.patch -text")
    notes = sorted((root / "release").glob("*.md"))
    if not notes:
        raise ValueError("No release notes")
    for path in notes:
        # Historical releases each keep their own original published checksums.
        text = path.read_text(encoding="utf-8-sig")
        if text.startswith(MACHINE_PREFIX):
            data = check_announcement(text)
            if path.stem != data["version"][1:]:
                raise ValueError(f"{path.name}: file name differs from version {data['version']}")
        assets = json.loads(path.with_suffix(".assets.json").read_text(encoding="utf-8"))
        # Since 0.32.1 the notes list only the two ZIPs; source members stay in <version>.sources.SHA256SUMS.txt.
        source_rows = None if text.startswith(MACHINE_PREFIX) else path.with_suffix(".sources.SHA256SUMS.txt")
        check_notes(path, path.with_suffix(".SHA256SUMS.txt"), {name: a["bytes"] for name, a in assets.items()}, source_rows)
        # The size manifest and both checksum files must also agree, independently of notes.
        lines = path.with_suffix(".SHA256SUMS.txt").read_text().splitlines() + path.with_suffix(".sources.SHA256SUMS.txt").read_text().splitlines()
        hashes = dict((line.split("  ")[1], line.split("  ")[0]) for line in lines)
        if hashes != {name: a["sha256"] for name, a in assets.items()}:
            raise ValueError("Size manifest hashes differ from checksum files")


def replay(root, work):
    if work.exists():
        raise ValueError("Replay workspace already exists; choose a fresh directory")
    work.mkdir(parents=True)
    series = json.loads((root / "patches/series.json").read_text(encoding="utf-8"))
    base_text = (root / "patches/BASE.md").read_text(encoding="utf-8")
    summary = []
    for s in series:
        if not re.fullmatch(r"platform/[a-zA-Z0-9_/-]+", s["upstream"]) or any(not re.fullmatch(r"[0-9a-f]{40}", s[k]) for k in ["base", "head", "tree"]):
            raise ValueError("Invalid AOSP project/base")
        base_rows = [line for line in base_text.splitlines() if f"`{s['dir']}`" in line and line.startswith("|")]
        if len(base_rows) != 1 or any(value not in base_rows[0] for value in [s["base"], s["head"], s["upstream"]]):
            raise ValueError("series.json and BASE.md differ")
        if int(base_rows[0].split("|")[4].strip()) != s["count"]:
            raise ValueError("series.json and BASE.md patch counts differ")
        directory = s["dir"]
        if not re.fullmatch(r"[a-z0-9_-]+/[a-z0-9_-]+", directory):
            raise ValueError("Unsafe series directory")
        repo = work / directory.replace("/", "-")
        repo.mkdir()
        git(repo, "init", "-q")
        git(repo, "config", "user.name", "qiyinxi")
        git(repo, "config", "user.email", "qiyinxi@foxmail.com")
        git(repo, "config", "gc.auto", "0")
        git(repo, "fetch", "--no-tags", "--depth=1", "https://android.googlesource.com/" + s["upstream"], s["base"])
        git(repo, "checkout", "-q", "--detach", "FETCH_HEAD")
        patches = sorted((root / "patches" / directory).glob("*.patch"))
        if len(patches) != s["count"] or not patches:
            raise ValueError("Patch count differs")
        git(repo, "am", "--keep-cr", "-q", *patches)
        count = int(git(repo, "rev-list", "--count", s["base"] + "..HEAD"))
        tree = git(repo, "rev-parse", "HEAD^{tree}")
        if count != s["count"] or tree != s["tree"]:
            raise ValueError(f"Replayed count/tree differs: {directory}")
        authors = set(git(repo, "log", "--format=%an <%ae>", s["base"] + "..HEAD").splitlines())
        messages = git(repo, "log", "--format=%B", s["base"] + "..HEAD")
        if authors != {"qiyinxi <qiyinxi@foxmail.com>"} or re.search(r"(?i)co-authored-by:", messages):
            raise ValueError("Patch authors/trailers differ")
        summary.append({"series": directory, "commits": count, "tree": tree, "depth": 1})
        print(f"Clean AOSP shallow replay: {directory}: {count} patches; tree {tree}", flush=True)
    (work / "result.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["metadata", "patches"])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--work", type=Path)
    args = parser.parse_args()
    args.root = args.root.resolve()
    try:
        if args.command == "metadata":
            metadata(args.root)
        else:
            if args.work is None:
                parser.error("patches requires --work (a fresh directory)")
            replay(args.root, args.work)
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
