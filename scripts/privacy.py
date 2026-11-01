"""Fail-closed privacy scanner shared by release packaging and public CI.

Exceptions pin a complete printable string and either the file SHA-256 or the
exact upstream file path. New strings still fail; no surname-wide exemption.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile

# Only generic rules live in this file. Rules that name private identifiers
# (real name, private email, hostname, local directories) sit in a
# privacy-private-rules.json that never enters this repository: the maintainer
# points MODAVD_PRIVATE_RULES (or --private-rules, or use_rules()) at it.
# Public CI runs without them; the release entry point refuses to scan unless
# they were loaded.
PUBLIC_RULES = {
    # name: (pattern, UTF-16 prefilter prefixes)
    "user-path": (r"[a-z]:[\\/]+Users[\\/]+[^\\/\s\"'<>|]+", ["Users"]),
}
PRIVATE_RULES_ENV = "MODAVD_PRIVATE_RULES"


def load_rule_specs(private_file):
    """The public rules plus those in `private_file` (None: public rules only). A named file must exist."""
    specs = dict(PUBLIC_RULES)
    if private_file is None:
        return specs, False
    private_file = Path(private_file)
    if not private_file.is_file():
        raise FileNotFoundError(f"Private privacy rules not found: {private_file}")
    for name, spec in json.loads(private_file.read_text(encoding="utf-8"))["rules"].items():
        specs[name] = (spec["pattern"], spec["prefixes"])
    return specs, True


def use_rules(private_file):
    """Build the matchers from the public rules plus `private_file` (None: public rules only)."""
    global RULE_SPECS, PRIVATE_RULES_LOADED, PRIVATE_RULES_FILE, RULES, ANY_RULE, ASCII_CANDIDATE, U16_CANDIDATE, NEEDLES
    RULE_SPECS, PRIVATE_RULES_LOADED = load_rule_specs(private_file)
    PRIVATE_RULES_FILE = Path(private_file) if private_file is not None else None
    RULES = {name: re.compile(pattern, re.I) for name, (pattern, _) in RULE_SPECS.items()}
    ANY_RULE = re.compile("|".join(rx.pattern for rx in RULES.values()), re.I)
    ASCII_CANDIDATE = re.compile(ANY_RULE.pattern.encode("ascii"), re.I)
    # UTF-16 prefix prefilter; full matching still uses RULES on decoded printable strings.
    PREFIXES = [prefix for _, prefixes in RULE_SPECS.values() for prefix in prefixes]
    U16_CANDIDATE = re.compile(b"|".join(re.escape(s.encode("utf-16le")) for s in PREFIXES), re.I)
    NEEDLES = [s.lower().encode(e) for s in PREFIXES for e in ["ascii", "utf-16le"]]


use_rules(os.environ.get(PRIVATE_RULES_ENV) or None)


def strings(data):
    # Find candidate strings directly, without allocating every string in large executables.
    for rx, encoding, width in [(ASCII_CANDIDATE, "ascii", 1), (U16_CANDIDATE, "utf-16le", 2)]:
        seen = set()
        for match in rx.finditer(data):
            lo, hi = match.span()
            if width == 1:
                while lo > 0 and 32 <= data[lo-1] <= 126:
                    lo -= 1
                while hi < len(data) and 32 <= data[hi] <= 126:
                    hi += 1
            else:
                while lo >= 2 and data[lo-1] == 0 and 32 <= data[lo-2] <= 126:
                    lo -= 2
                while hi+1 < len(data) and data[hi+1] == 0 and 32 <= data[hi] <= 126:
                    hi += 2
            if (lo, hi) not in seen:
                seen.add((lo, hi))
                yield data[lo:hi].decode(encoding)
    try:
        text = data.decode("utf-8-sig")
    except UnicodeDecodeError:
        return
    for line in text.splitlines():
        if any(ord(c) > 127 for c in line):
            yield line


def scan_files(files, allowlist):
    rules = json.loads(Path(allowlist).read_text(encoding="utf-8"))
    allowed_keys = {(e["file_sha256"], e["rule"], e["string_sha256"]) for e in rules["exceptions"] if "file_sha256" in e}
    allowed_paths = {(e["path"], e["rule"], e["string_sha256"]) for e in rules["exceptions"] if "path" in e}
    report = {"files_scanned": 0, "failures": [], "allowed": []}
    def blob(name, data):
        report["files_scanned"] += 1
        lower = data.lower()
        if not any(needle in lower for needle in NEEDLES):
            return
        file_sha = None
        logical_path = name.split("!", 1)[-1]
        if logical_path.startswith("ModAVD-") and "/" in logical_path:
            logical_path = logical_path.split("/", 1)[1]
        seen = set()
        for string in strings(data):
            if not ANY_RULE.search(string):
                continue
            for rule, rx in RULES.items():
                if not rx.search(string):
                    continue
                digest = hashlib.sha256(string.encode("utf-8")).hexdigest()
                if (rule, digest) in seen:
                    continue
                seen.add((rule, digest))
                if file_sha is None:
                    file_sha = hashlib.sha256(data).hexdigest()
                entry = {"file": name, "path": logical_path, "rule": rule, "file_sha256": file_sha, "string_sha256": digest}
                if (file_sha, rule, digest) in allowed_keys or (logical_path, rule, digest) in allowed_paths:
                    report["allowed"].append(entry)
                else:
                    matches = list(rx.finditer(string))
                    entry["sample"] = " | ".join(string[max(0, m.start()-80):m.end()+120] for m in matches)
                    report["failures"].append(entry)
    for path, name in files:
        path = Path(path)
        if name.endswith((".tar.xz", ".tar.gz")):
            print(f"Scanning source members: {name}", flush=True)
            with tarfile.open(path, "r|*") as archive:
                for member in archive:
                    if member.isfile():
                        blob(name + "!" + member.name, archive.extractfile(member).read())
        else:
            blob(name, path.read_bytes())
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--allowlist", type=Path, default=Path(__file__).with_name("privacy-allowlist.json"))
    parser.add_argument("--report", type=Path)
    parser.add_argument("--private-rules", type=Path, help=f"private rules file (default: ${PRIVATE_RULES_ENV}, else none)")
    args = parser.parse_args()
    if args.private_rules:
        use_rules(args.private_rules)
    # CI scans tracked repository files, never .git internals or arbitrary local artifacts.
    names = subprocess.check_output(["git", "-C", str(args.root), "ls-files", "-z"]).decode("utf-8").split("\0")
    files = [(args.root / name, name) for name in names if name]
    report = scan_files(files, args.allowlist)
    if args.report:
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    private = f"private rules {PRIVATE_RULES_FILE}" if PRIVATE_RULES_LOADED else "public rules only"
    print(f"Privacy: {report['files_scanned']} files; {len(report['allowed'])} allowed; {len(report['failures'])} failures ({private})")
    for item in report["failures"][:20]:
        print(f"  {item['file']}: {item['rule']}: {item['sample'][:120]}")
    return bool(report["failures"])


if __name__ == "__main__":
    sys.exit(main())
