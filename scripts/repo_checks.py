"""Validate commit authors and Co-authored-by trailers for public history.

This file and test_repo_checks.py are the same in every ModAVD repository. Change them in ModAVD/ModAVD first, then
copy them byte for byte to the others; each repository's CI compares them with ModAVD/ModAVD main.
"""
from __future__ import annotations

import os
import re
import subprocess
import sys

ALLOWED_AUTHOR = re.compile(r"^qiyinxi <qiyinxi@foxmail\.com>$")
ALLOWED_NOREPLY = (
    re.compile(r"^[0-9]+\+[A-Za-z0-9-]+(?:\[bot\])?@users\.noreply\.github\.com$"),
    re.compile(r"^[A-Za-z0-9-]+@users\.noreply\.github\.com$"),
)


def allowed(name: str, email: str) -> bool:
    identity = f"{name} <{email}>"
    return bool(ALLOWED_AUTHOR.fullmatch(identity) or any(rx.fullmatch(email) for rx in ALLOWED_NOREPLY))


def commits(base: str, head: str, cwd: str = ".") -> list[str]:
    spec = f"{base}..{head}"
    out = subprocess.check_output(["git", "rev-list", "--no-merges", spec], cwd=cwd, text=True, encoding="utf-8")
    return [x for x in out.splitlines() if x]


def check_commit(commit: str, cwd: str = ".") -> list[str]:
    fmt = "%an%x00%ae%x00%B"
    raw = subprocess.check_output(["git", "show", "-s", f"--format={fmt}", commit], cwd=cwd, text=True,
                                  encoding="utf-8")
    name, email, message = raw.split("\x00", 2)
    errors: list[str] = []
    if not allowed(name.strip(), email.strip()):
        errors.append(f"{commit}: author {name.strip()} <{email.strip()}> is not allowed")
    for line in message.splitlines():
        if line.strip().lower().startswith("co-authored-by:"):
            value = line.split(":", 1)[1].strip()
            match = re.fullmatch(r".+ <([^<>]+)>", value)
            if not match or not allowed(value.rsplit(" <", 1)[0], match.group(1)):
                errors.append(f"{commit}: {line}")
    return errors


def main() -> int:
    base = os.environ.get("REPO_CHECK_BASE")
    head = os.environ.get("REPO_CHECK_HEAD")
    if not base or not head:
        base, head = "origin/main", "HEAD"
    try:
        ids = commits(base, head)
    except subprocess.CalledProcessError as exc:
        print(f"cannot inspect {base}..{head}: git exited {exc.returncode}")
        return 1
    errors = [error for commit in ids for error in check_commit(commit)]
    for error in errors:
        print(error)
    print(f"Author check: {len(ids)} non-merge commits; {len(errors)} failures")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
