"""Focused failure-path checks. Run with --work pointing to a new task temp dir."""
import argparse
import contextlib
import hashlib
import importlib
import io
import json
import os
from pathlib import Path
import re
import sys
import tarfile
from types import SimpleNamespace
import unittest
from unittest import mock
import xml.etree.ElementTree as ET
import zipfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "scripts"))
sys.path.insert(0, str(HERE))
import checks
import privacy
import release_tools as release

WORK = None
# Stand-ins for the maintainer's private rules (which never enter this repository): made-up names, so this file
# passes the real rules too.
TEST_RULES = {"rules": {"realname": {"pattern": "Qwerzy|Plonkett", "prefixes": ["Qwerzy", "Plonkett"]}}}


class ReleaseChecks(unittest.TestCase):
    def folder(self):
        p = WORK / self._testMethodName
        p.mkdir(parents=True)
        return p

    @contextlib.contextmanager
    def private_rules(self, rules=TEST_RULES):
        """Scan with the public rules plus `rules`, as privacy.py does with the maintainer's file; restore afterwards."""
        path = WORK / f"{self._testMethodName}.rules.json"
        path.write_text(json.dumps(rules), encoding="utf-8")
        previous = privacy.PRIVATE_RULES_FILE
        privacy.use_rules(path)
        try:
            yield path
        finally:
            privacy.use_rules(previous)

    def test_relative_paths_cannot_escape(self):
        for value in ["../other", "a/../../other", "C:/source", "/source", "a\\other"]:
            with self.assertRaises(ValueError):
                release.safe_rel(value)

    def test_release_checkout_refused_in_protected_roots(self):
        root = self.folder()
        source, backup = root / "src", root / "backup"
        for p in [source / "external/qemu", backup]:
            p.mkdir(parents=True)
        self.commit_files(root / "pub", {"README.md": b"# ModAVD\n", "patches/BASE.md": b"x\n"})
        protected = [str(source), str(backup)]
        self.assertEqual(release.release_repository(root / "pub", protected), (root / "pub").resolve())
        for value in [source, source / "external/qemu", backup]:
            with self.assertRaisesRegex(ValueError, "source, backup or production"):
                release.release_repository(value, protected)
        self.assertEqual(release.inside(source / "a/b", protected), str(source))
        self.assertIsNone(release.inside(root / "src2", protected))
        if sys.platform == "win32":
            # drive roots and case: NTFS ignores case
            self.assertEqual(release.inside(str(source).upper(), protected), str(source))
            self.assertTrue(release.inside(source, [source.anchor]))

    def test_uncommitted_check_uses_the_checkouts_line_endings(self):
        """A clone made with core.autocrlf=true has its text files in CRLF. Until its index is refreshed git compares
        their content, and under GIT_ENV's core.autocrlf=false each of them would count as modified."""
        root = self.folder()
        notice_rel = "tools/pubrel/config/own-sources-NOTICE.txt"
        self.commit_files(root / "pub", {notice_rel: b"line one\nline two\n", "package/LICENSE": b"text\n",
                                         ".gitattributes": b"package/** -text\n"})
        clone = root / "clone"
        release.run("git", "-c", "core.autocrlf=true", "clone", "-q", "-c", "core.autocrlf=true", root / "pub", clone,
                    env=release.STATUS_ENV)
        notice = clone / notice_rel
        self.assertIn(b"\r\n", notice.read_bytes())
        # make git compare the content instead of trusting the index, as it does for a fresh clone
        later = notice.stat().st_mtime + 60
        os.utime(notice, (later, later))
        self.assertTrue(release.git(clone, "status", "--porcelain", "--", notice_rel))
        self.assertEqual(release.changed(clone, notice_rel, "package/LICENSE"), "")
        # real changes are still reported
        notice.write_bytes(notice.read_bytes() + b"more\r\n")
        self.assertIn("own-sources-NOTICE.txt", release.changed(clone, notice_rel))
        (clone / "package/new.ps1").write_bytes(b"x")
        self.assertIn("package/new.ps1", release.changed(clone, "package"))

    def test_force_requires_exact_ownership(self):
        path = self.folder()
        args = SimpleNamespace(output=str(path), build="mas30")
        with self.assertRaises(FileNotFoundError):
            release.owned(args)
        release.owned(args, True)
        release.owned(args)
        with self.assertRaises(ValueError):
            release.owned(SimpleNamespace(output=str(path), build="mas31"))

    def test_privacy_blocks_ascii_unicode_and_archive_members(self):
        path = self.folder()
        allow = path / "allow.json"
        allow.write_text('{"exceptions": []}', encoding="utf-8")
        # Construct sensitive examples so the public repository can scan this test file too.
        person = "Qwer" + "zy"
        payload = (("C:" + chr(92) + "Users" + chr(92) + "localperson" + chr(92) + "文档") + "\n" + person).encode("utf-8")
        (path / "plain").write_bytes(payload)
        (path / "wide").write_bytes(person.encode("utf-16le"))
        with tarfile.open(path / "source.tar.xz", "w:xz") as tar:
            info = tarfile.TarInfo("upstream/author.txt")
            info.size = len(payload)
            tar.addfile(info, io.BytesIO(payload))
        with self.private_rules():
            report = privacy.scan_files([(path / n, n) for n in ["plain", "wide", "source.tar.xz"]], allow)
        self.assertEqual(report["files_scanned"], 3)
        self.assertTrue(any(e["file"] == "wide" and e["rule"] == "realname" for e in report["failures"]))
        self.assertTrue(any("!" in e["file"] and e["rule"] == "user-path" for e in report["failures"]))

    def test_privacy_ascii_fallback_keeps_odd_length_usernames(self):
        path = self.folder()
        file, allow = path / "binary", path / "allow.json"
        allow.write_text('{"exceptions": []}', encoding="utf-8")
        for user in ["a", "ab", "abc"]:
            text = " C:" + chr(92) + "Users" + chr(92) + user
            with self.subTest(user=user):
                file.write_bytes(text.encode() + b"\x00\x90")
                report = privacy.scan_files([(file, file.name)], allow)
                self.assertIn(hashlib.sha256(text.encode()).hexdigest(),
                              [e["string_sha256"] for e in report["failures"]])

    def test_privacy_unicode_users_in_binary_at_both_alignments(self):
        path = self.folder()
        file, allow = path / "binary", path / "allow.json"
        allow.write_text('{"exceptions": []}', encoding="utf-8")
        for user in ["张三", "éclair", "🚀", "𝄞", "محمد", "A张三"]:
            text = "C:" + chr(92) + "Users" + chr(92) + user + chr(92) + "project"
            for encoding in ["utf-8", "utf-16le"]:
                for prefix in [b"\x00\x00", b"\x90\x00\x00", b"\xff\xfe"]:
                    with self.subTest(user=user, encoding=encoding, prefix=prefix):
                        file.write_bytes(prefix + text.encode(encoding) + b"\x00\x00\xfe")
                        report = privacy.scan_files([(file, file.name)], allow)
                        self.assertTrue(any(user in e["sample"] for e in report["failures"]))

    def test_privacy_gbk_usernames_in_binary_are_reported(self):
        path = self.folder()
        file, allow = path / "binary", path / "allow.json"
        allow.write_text('{"exceptions": []}', encoding="utf-8")
        for user in ["张三", "A张三"]:
            for suffix in ["", chr(92) + "project"]:
                text = "C:" + chr(92) + "Users" + chr(92) + user + suffix
                data = text.encode("gbk")
                with self.subTest(user=user, suffix=suffix):
                    file.write_bytes(b"\x90\x00" + data + b"\x00\xff")
                    report = privacy.scan_files([(file, file.name)], allow)
                    self.assertTrue(any(e["rule"] == "user-path" and data.decode("latin-1") in e["sample"]
                                        for e in report["failures"]))

    def test_privacy_invalid_unicode_sequences_bound_candidates(self):
        text = "C:" + chr(92) + "Users" + chr(92) + "张三" + chr(92) + "project"
        boundaries = {
            "utf-8": [(b"\xff", b"\xff"), (b"\x80", b"\xc0\xaf"),
                      (b"\xed\xa0\x80", b"\xf4\x90\x80\x80"), (b"\xe2\x82", b"\xe2\x82"),
                      (b"\x01", b"\xc2\x80")],
            "utf-16le": [(b"\x00\xd8", b"\x00\xdc"), (b"\xff\xfe", b"\x00\x00"),
                         (b"\x00\x00", b"\x80\x00"), (b"\x00\x00", b"\x00\xd8"),
                         (b"\x00\x00", b"\xff")],
        }
        for encoding, cases in boundaries.items():
            for prefix, suffix in cases:
                with self.subTest(encoding=encoding, prefix=prefix, suffix=suffix):
                    candidates = list(privacy.strings(prefix + text.encode(encoding) + suffix))
                    self.assertIn(text, candidates)
                    self.assertFalse(any("\ufffd" in candidate for candidate in candidates))

    def test_privacy_ascii_boundaries_and_allowlist_digests_stay_exact(self):
        path = self.folder()
        file, allow = path / "binary", path / "allow.json"
        text = "prefix C:" + chr(92) + "Users" + chr(92) + "Example" + chr(92) + "project tail"
        for encoding in ["ascii", "utf-16le"]:
            with self.subTest(encoding=encoding):
                data = b"\x00\xff\x01\x00" + text.encode(encoding) + b"\x00\x00\xff"
                file.write_bytes(data)
                allow.write_text('{"exceptions": []}', encoding="utf-8")
                report = privacy.scan_files([(file, file.name)], allow)
                expected = {"file": file.name, "path": file.name, "rule": "user-path",
                            "file_sha256": hashlib.sha256(data).hexdigest(),
                            "string_sha256": hashlib.sha256(text.encode()).hexdigest(), "sample": text}
                self.assertEqual(report["failures"], [expected])
                allow.write_text(json.dumps({"exceptions": [expected]}), encoding="utf-8")
                allowed = privacy.scan_files([(file, file.name)], allow)
                self.assertEqual(allowed["failures"], [])
                self.assertEqual(allowed["allowed"], [{k: v for k, v in expected.items() if k != "sample"}])

    def test_privacy_utf16_prefix_on_other_alignment_does_not_hide_path(self):
        path = self.folder()
        file, allow = path / "binary", path / "allow.json"
        allow.write_text('{"exceptions": []}', encoding="utf-8")
        text = "C:" + chr(92) + "Users" + chr(92) + "张三"
        # The odd-offset prefix can decode across the even-offset path as unrelated printable code points.
        file.write_bytes(b"\x90" + "Users".encode("utf-16le") + b"\x00" + text.encode("utf-16le") + b"\x00\x00")
        report = privacy.scan_files([(file, file.name)], allow)
        self.assertTrue(any(text in e["sample"] for e in report["failures"]))

    def test_privacy_utf8_prefixes_follow_rule_changes(self):
        self.folder()
        for prefix in ["Token:", "Marker:"]:
            rules = {"rules": {"token": {"pattern": re.escape(prefix) + r"[^\s\x00]+", "prefixes": [prefix]}}}
            text = prefix + "张三"
            data = b"\xff\x00" + text.encode() + b"\x00\xfe"
            with self.subTest(prefix=prefix), self.private_rules(rules):
                self.assertIn(text, list(privacy.strings(data)))
            self.assertNotIn(text, list(privacy.strings(data)))

    def test_privacy_unicode_allowlist_matches_complete_string(self):
        path = self.folder()
        file, allow = path / "binary", path / "allow.json"
        text = "C:" + chr(92) + "Users" + chr(92) + "张三"
        for encoding in ["utf-8", "utf-16le"]:
            with self.subTest(encoding=encoding):
                file.write_bytes(b"\x00\x00" + text.encode(encoding) + b"\x00\x00")
                allow.write_text('{"exceptions": []}', encoding="utf-8")
                report = privacy.scan_files([(file, file.name)], allow)
                self.assertTrue(report["failures"])
                entries = [{k: v for k, v in e.items() if k != "file_sha256"} for e in report["failures"]]
                allow.write_text(json.dumps({"exceptions": entries}), encoding="utf-8")
                self.assertEqual(privacy.scan_files([(file, file.name)], allow)["failures"], [])
                file.write_bytes(b"\x00\x00" + (text + "x").encode(encoding) + b"\x00\x00")
                self.assertTrue(privacy.scan_files([(file, file.name)], allow)["failures"])

    def test_private_rules_loaded_from_outside_or_release_scan_stops(self):
        """The private rules come from a file outside the repository (--private-rules / MODAVD_PRIVATE_RULES); a
        named file that is missing is an error, and the release scan refuses to run with the public rules only."""
        path = self.folder()
        out = path / "out"
        for scope in ["release", "patches", "source"]:
            (out / scope).mkdir(parents=True)
        (out / "release/notes.txt").write_text("Third Party " + "Plon" + "kett", encoding="utf-8")
        for name in ["SHA256SUMS.txt", "release-assets.json", "release-notes.md"]:
            (out / name).write_text("x\n", encoding="utf-8")
        args = SimpleNamespace(output=str(out), private_rules=None)
        previous, env = privacy.PRIVATE_RULES_FILE, os.environ.get(privacy.PRIVATE_RULES_ENV)
        try:
            privacy.use_rules(None)
            self.assertFalse(privacy.PRIVATE_RULES_LOADED)
            with self.assertRaisesRegex(ValueError, "not loaded"):
                release.privacy_release(args)
            self.assertFalse((out / "logs/privacy.json").exists())
            with self.assertRaises(FileNotFoundError):
                privacy.use_rules(path / "missing.json")
            rules = path / "elsewhere/private-rules.json"
            rules.parent.mkdir()
            rules.write_text(json.dumps(TEST_RULES), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Privacy failures"):
                release.privacy_release(SimpleNamespace(output=str(out), private_rules=str(rules)))
            self.assertTrue(privacy.PRIVATE_RULES_LOADED)
            self.assertEqual(release.load(out / "logs/privacy.json")["failures"][0]["rule"], "realname")
            # the environment variable is read on import
            privacy.use_rules(None)
            os.environ[privacy.PRIVATE_RULES_ENV] = str(rules)
            importlib.reload(privacy)
            self.assertEqual((privacy.PRIVATE_RULES_LOADED, privacy.PRIVATE_RULES_FILE), (True, rules))
        finally:
            if env is None:
                os.environ.pop(privacy.PRIVATE_RULES_ENV, None)
            else:
                os.environ[privacy.PRIVATE_RULES_ENV] = env
            privacy.use_rules(previous)

    def cache_fixture(self, path, version):
        """A release-like file list: two package files (one with a reviewed exception by path), an archive (one member
        with a reviewed exception by file hash) and release notes with a real failure. Returns (files, allowlist)."""
        user = "C:" + chr(92) + "Users" + chr(92) + "localperson"
        person = "Qwer" + "zy"
        package = path / version / f"ModAVD-{version}-win-x64"
        for rel, data in {"sdk/a.dll": b"\x00nothing to see\x00", "sdk/b.dll": (user + "\n").encode()}.items():
            (package / rel).parent.mkdir(parents=True, exist_ok=True)
            (package / rel).write_bytes(data)
        (path / version / "release-notes.md").write_text(f"written by {person}\n", encoding="utf-8")
        member = (user + chr(92) + "build\n").encode()
        archive = path / "source.tar.xz"
        if not archive.exists():
            with tarfile.open(archive, "w:xz") as tar:
                for name, data in [("upstream/clean.txt", b"clean\n"), ("upstream/built.txt", member)]:
                    info = tarfile.TarInfo(name)
                    info.size = len(data)
                    tar.addfile(info, io.BytesIO(data))
        digest = lambda s: hashlib.sha256(s.encode()).hexdigest()
        allow = path / "allow.json"
        if not allow.exists():
            allow.write_text(json.dumps({"exceptions": [
                {"path": "sdk/b.dll", "rule": "user-path", "string_sha256": digest(user)},
                {"file_sha256": hashlib.sha256(member).hexdigest(), "rule": "user-path", "string_sha256": digest(user + chr(92) + "build")}]}),
                encoding="utf-8")
        files = [(package / "sdk/a.dll", f"ModAVD-{version}-win-x64/sdk/a.dll"), (package / "sdk/b.dll", f"ModAVD-{version}-win-x64/sdk/b.dll"),
                 (archive, "source.tar.xz"), (path / version / "release-notes.md", "release-notes.md")]
        return files, allow

    def scan_counting(self, files, allow, cache, write_only=False):
        """cached_privacy_scan, recording which files went to privacy.scan_files."""
        scanned = []
        real = privacy.scan_files
        def spy(batch, allowlist):
            scanned.extend(name for _, name in batch)
            return real(batch, allowlist)
        with mock.patch.object(privacy, "scan_files", spy):
            report = release.cached_privacy_scan(privacy, files, allow, cache, write_only=write_only)
        return report, scanned

    @staticmethod
    def without_version(report):
        strip = lambda e: {**e, "file": re.sub(r"^ModAVD-[^/]+/", "", e["file"])}
        return (report["files_scanned"], sorted(json.dumps(strip(e), sort_keys=True) for e in report["allowed"]),
                sorted(json.dumps(strip(e), sort_keys=True) for e in report["failures"]))

    def test_privacy_cache_skips_unchanged_clean_files(self):
        """0.38.0 scanned 380 763 files and members several times. The release scan now skips what the last one found
        clean and what has not changed since; the report is the same as a full scan's."""
        path = self.folder()
        cache = path / "cache/release.json"
        with self.private_rules():
            files, allow = self.cache_fixture(path, "0.38.0")
            full = privacy.scan_files([(p, n) for p, n in files], allow)
            first, scanned = self.scan_counting(files, allow, cache)
            self.assertEqual(sorted(scanned), sorted(n for _, n in files))
            self.assertEqual(self.without_version(first), self.without_version(full))
            self.assertEqual(full["files_scanned"], 5)
            self.assertEqual([e["file"] for e in full["failures"]], ["release-notes.md"])
            self.assertEqual(len(full["allowed"]), 2)
            # the next release: same files under the new package folder; only the one with a failure is scanned again
            files, _ = self.cache_fixture(path, "0.39.0")
            second, scanned = self.scan_counting(files, allow, cache)
            self.assertEqual(scanned, ["release-notes.md"])
            self.assertEqual(self.without_version(second), self.without_version(full))
            self.assertEqual(second["cache"]["hits"], 3)
            self.assertEqual(sorted(e["file"] for e in second["allowed"]), ["ModAVD-0.39.0-win-x64/sdk/b.dll", "source.tar.xz!upstream/built.txt"])
            # a changed file is scanned again
            files[0][0].write_bytes(b"\x00changed\x00")
            _, scanned = self.scan_counting(files, allow, cache)
            self.assertEqual(sorted(scanned), sorted([files[0][1], "release-notes.md"]))
            # an exception taken out of the allowlist: the file that used it is scanned again, and fails
            rules = json.loads(allow.read_text(encoding="utf-8"))
            narrowed = path / "allow-narrowed.json"
            narrowed.write_text(json.dumps({"exceptions": rules["exceptions"][1:]}), encoding="utf-8")
            third, scanned = self.scan_counting(files, narrowed, cache)
            self.assertEqual(sorted(scanned), sorted([files[1][1], "release-notes.md"]))
            self.assertIn(files[1][1], [e["file"] for e in third["failures"]])
            # different rules drop the whole cache
            with self.private_rules({"rules": {"realname": {"pattern": "Plonkett", "prefixes": ["Plonkett"]}}}):
                fourth, scanned = self.scan_counting(files, allow, cache)
            self.assertEqual(sorted(scanned), sorted(n for _, n in files))
            self.assertIn("changed", fourth["cache"]["note"])
            # an unreadable cache is dropped too
            cache.write_text("{not json", encoding="utf-8")
            fifth, scanned = self.scan_counting(files, allow, cache)
            self.assertEqual(sorted(scanned), sorted(n for _, n in files))
            self.assertIn("unreadable", fifth["cache"]["note"])
            # a changed scanner drops it as well: its code is part of the fingerprint
            changed = path / "privacy.py"
            changed.write_bytes(Path(privacy.__file__).read_bytes() + b"\n# fixed a miss\n")
            other = SimpleNamespace(__file__=str(changed), RULE_SPECS=privacy.RULE_SPECS)
            self.assertNotEqual(release.privacy_fingerprint(other), release.privacy_fingerprint(privacy))
            self.assertEqual(release.privacy_fingerprint(SimpleNamespace(__file__=privacy.__file__, RULE_SPECS=dict(privacy.RULE_SPECS))),
                             release.privacy_fingerprint(privacy))

    def test_privacy_release_cache_stays_outside_output_and_checkout(self):
        path = self.folder()
        out = path / "out"
        for scope in ["release", "patches", "source"]:
            (out / scope).mkdir(parents=True)
        for name in ["SHA256SUMS.txt", "release-assets.json", "release-notes.md"]:
            (out / name).write_text("x\n", encoding="utf-8")
        with self.private_rules() as rules:
            for bad in [out / "privacy-cache.json", release.REPO / "privacy-cache.json"]:
                with self.assertRaisesRegex(ValueError, "outside the release output"):
                    release.privacy_release(SimpleNamespace(output=str(out), private_rules=str(rules), privacy_cache=str(bad)))
            release.privacy_release(SimpleNamespace(output=str(out), private_rules=str(rules), privacy_cache=str(path / "cache.json")))
            self.assertEqual(release.load(out / "logs/privacy.json")["cache"]["scanned"], 3)
            release.privacy_release(SimpleNamespace(output=str(out), private_rules=str(rules), privacy_cache=str(path / "cache.json")))
            self.assertEqual(release.load(out / "logs/privacy.json")["cache"]["hits"], 3)
            # the run that uploads (release.ps1 -DraftRelease passes --privacy-cache-write-only) scans everything again,
            # and still leaves a cache the next run hits
            release.privacy_release(SimpleNamespace(output=str(out), private_rules=str(rules), privacy_cache=str(path / "cache.json"),
                                                    privacy_cache_write_only=True))
            report = release.load(out / "logs/privacy.json")["cache"]
            self.assertEqual((report["hits"], report["scanned"]), (0, 3))
            self.assertIn("upload", report["note"])
            release.privacy_release(SimpleNamespace(output=str(out), private_rules=str(rules), privacy_cache=str(path / "cache.json")))
            self.assertEqual(release.load(out / "logs/privacy.json")["cache"]["hits"], 3)

    def test_privacy_cache_is_not_trusted_by_the_upload_run(self):
        """A "clean" entry written in the cache's own format (by hand, or by a bug in the cache) skips a file that
        fails. Every run but the one that uploads accepts that (the cache is this machine's own input); the upload run
        does not read the cache at all."""
        path = self.folder()
        cache = path / "cache.json"
        with self.private_rules():
            files, allow = self.cache_fixture(path, "0.38.0")
            self.scan_counting(files, allow, cache)
            data = release.load(cache)
            notes = [(p, n) for p, n in files if n == "release-notes.md"][0]
            key = release.privacy_key(*notes)
            self.assertNotIn(key, data["entries"])
            data["entries"][key] = {"files": 1, "allowed": []}
            cache.write_text(json.dumps(data), encoding="utf-8")
            forged, scanned = self.scan_counting(files, allow, cache)
            self.assertEqual((scanned, forged["failures"]), ([], []))
            upload, scanned = self.scan_counting(files, allow, cache, write_only=True)
            self.assertEqual(sorted(scanned), sorted(n for _, n in files))
            self.assertEqual([e["file"] for e in upload["failures"]], ["release-notes.md"])
            self.assertEqual(upload["cache"]["hits"], 0)
            # the upload run rewrote the cache from its own scan: the forged entry is gone, the clean ones are back
            self.assertNotIn(key, release.load(cache)["entries"])
            _, scanned = self.scan_counting(files, allow, cache)
            self.assertEqual(scanned, ["release-notes.md"])
        # release.ps1 passes the flag with -DraftRelease, inside step 8; and the command line takes it
        ps = (HERE / "release.ps1").read_text(encoding="utf-8")
        step8 = ps[ps.index("Step 8 "):ps.index("\nif ($DraftRelease) {", ps.index("Step 8 "))]
        self.assertRegex(step8, r"if \(\$DraftRelease -and \$PrivacyCache\) \{ \$cacheArgs \+= '--privacy-cache-write-only' \}")
        self.assertLess(step8.index("--privacy-cache-write-only"), step8.index("Run-Python"))
        seen = []
        for argv, want in [(["--privacy-cache-write-only"], True), ([], False)]:
            with mock.patch.object(release, "privacy_release", seen.append), \
                    mock.patch.object(sys, "argv", ["release_tools.py", "privacy-release", "--output", str(path)] + argv):
                self.assertEqual(release.main(), 0)
            self.assertIs(seen[-1].privacy_cache_write_only, want)

    def test_privacy_cache_drops_malformed_entries(self):
        """An entry not shaped as the cache writes it is dropped and its file scanned again; the step does not fail."""
        path = self.folder()
        cache = path / "cache.json"
        with self.private_rules():
            files, allow = self.cache_fixture(path, "0.38.0")
            self.scan_counting(files, allow, cache)
            data = release.load(cache)
            keys = {n: release.privacy_key(p, n) for p, n in files}
            b = keys[files[1][1]]
            self.assertEqual(sorted(data["entries"]), sorted([keys[files[0][1]], b, keys["source.tar.xz"]]))
            good = data["entries"][b]
            member = good["allowed"][0]
            for bad in ["clean", {"allowed": []}, {"files": "1", "allowed": []}, {"files": True, "allowed": []}, {"files": 0, "allowed": []},
                        {"files": 1}, {"files": 1, "allowed": "none"}, {"files": 1, "allowed": ["x"]},
                        {"files": 1, "allowed": [{k: v for k, v in member.items() if k != "member"}]},
                        {"files": 1, "allowed": [{**member, "path": None}]}]:
                data["entries"][b] = bad
                cache.write_text(json.dumps(data), encoding="utf-8")
                report, scanned = self.scan_counting(files, allow, cache)
                self.assertEqual(sorted(scanned), sorted([files[1][1], "release-notes.md"]), bad)
                self.assertIn("1 malformed", report["cache"]["note"], bad)
                self.assertEqual(report["cache"]["hits"], 2, bad)
                self.assertEqual(release.load(cache)["entries"][b], good, bad)
            # entries that are not an object at all: the whole cache is dropped, still no failure of the step
            data["entries"] = [["k", "v"]]
            cache.write_text(json.dumps(data), encoding="utf-8")
            report, scanned = self.scan_counting(files, allow, cache)
            self.assertEqual(sorted(scanned), sorted(n for _, n in files))
            self.assertIn("unreadable", report["cache"]["note"])

    def test_privacy_cache_key_keeps_the_path(self):
        """An exception by path covers that path only: the same content moved elsewhere is scanned again and fails."""
        path = self.folder()
        cache = path / "cache.json"
        with self.private_rules():
            files, allow = self.cache_fixture(path, "0.38.0")
            self.scan_counting(files, allow, cache)
            moved = files[1][0].with_name("c.dll")
            moved.write_bytes(files[1][0].read_bytes())
            files = [files[0], (moved, files[1][1].replace("b.dll", "c.dll"))] + files[2:]
            report, scanned = self.scan_counting(files, allow, cache)
            self.assertIn(files[1][1], scanned)
            self.assertIn(files[1][1], [e["file"] for e in report["failures"]])

    def test_privacy_fingerprint_follows_python_and_unicode(self):
        """What isprintable() accepts comes from the Unicode database: a new Python can change which strings are checked."""
        import unicodedata
        base = release.privacy_fingerprint(privacy)
        with mock.patch.object(unicodedata, "unidata_version", "1.0.0"):
            self.assertNotEqual(release.privacy_fingerprint(privacy), base)
        major, minor, micro = sys.version_info[:3]
        with mock.patch.object(sys, "version_info", (major, minor + 1, 0)):
            self.assertNotEqual(release.privacy_fingerprint(privacy), base)
        with mock.patch.object(sys, "version_info", (major, minor, micro + 1)):
            self.assertEqual(release.privacy_fingerprint(privacy), base)

    def test_allowlist_does_not_allow_modified_file(self):
        path = self.folder()
        allow = path / "allow.json"
        allow.write_text('{"exceptions": []}', encoding="utf-8")
        file = path / "author.txt"
        file.write_text("Third Party " + "Plon" + "kett", encoding="utf-8")
        with self.private_rules():
            first = privacy.scan_files([(file, file.name)], allow)
            entry = {k: first["failures"][0][k] for k in ["file_sha256", "rule", "string_sha256"]}
            allow.write_text(json.dumps({"exceptions": [entry]}), encoding="utf-8")
            self.assertFalse(privacy.scan_files([(file, file.name)], allow)["failures"])
            file.write_bytes(file.read_bytes() + b"\nextra")
            self.assertTrue(privacy.scan_files([(file, file.name)], allow)["failures"])

    def test_notes_reject_wrong_hash_size_duplicate_and_missing(self):
        path = self.folder()
        sums = path / "SHA256SUMS.txt"
        sums.write_text("a" * 64 + "  main.zip\n", encoding="utf-8")
        notes = path / "notes.md"
        row = "| `main.zip` | 1,024 字节 | `" + "a" * 64 + "` |\n"
        notes.write_text(row, encoding="utf-8")
        checks.check_notes(notes, sums, {"main.zip": 1024})
        for bad in [row.replace("a" * 64, "b" * 64), row.replace("1,024", "1,025"), row + row, ""]:
            notes.write_text(bad, encoding="utf-8")
            with self.assertRaises(ValueError):
                checks.check_notes(notes, sums, {"main.zip": 1024})

    def test_scoped_allowlist_allows_only_same_upstream_line(self):
        path = self.folder()
        allow = path / "allow.json"
        allow.write_text('{"exceptions": []}', encoding="utf-8")
        file = path / "author.txt"
        file.write_text("Third Party " + "Plon" + "kett", encoding="utf-8")
        with self.private_rules():
            first = privacy.scan_files([(file, file.name)], allow)
            entry = {k: first["failures"][0][k] for k in ["path", "rule", "string_sha256"]}
            allow.write_text(json.dumps({"exceptions": [entry]}), encoding="utf-8")
            file.write_bytes(file.read_bytes() + b"\nunrelated edit")
            self.assertFalse(privacy.scan_files([(file, file.name)], allow)["failures"])
            file.write_bytes(file.read_bytes() + ("\nQwer" + "zy").encode())
            self.assertTrue(privacy.scan_files([(file, file.name)], allow)["failures"])
            self.assertTrue(privacy.scan_files([(file, "different-path")], allow)["failures"])

    def package(self, path):
        """A tiny release tree with its manifest, as assemble leaves it."""
        data = {"build": "mas33", "version": "0.33.0", "upstream": "37.2.10", "min_automas": "5.7.0"}
        release.save(path / "snapshot.json", data)
        tree = path / "release/ModAVD-0.33.0-win-x64"
        (tree / "sdk/emulator").mkdir(parents=True)
        (tree / "sdk/emulator/data").write_bytes(b"correct")
        (tree / "说明.txt").write_text("x", encoding="utf-8")
        release.write_manifest(tree, data)
        (path / "removed-files.txt").write_text("excluded\n", encoding="utf-8")
        return tree

    def write_zip(self, archive, tree, prefix, override=None, raw_names=None):
        with zipfile.ZipFile(archive, "w") as z:
            for p in sorted(tree.rglob("*")):
                if p.is_file():
                    rel = p.relative_to(tree).as_posix()
                    info = zipfile.ZipInfo(prefix + rel)
                    # ZipInfo normalizes native separators on Windows; set raw names after construction.
                    info.filename = (raw_names or {}).get(rel, info.filename)
                    z.writestr(info, (override or {}).get(rel, p.read_bytes()))

    def test_zip_rejects_backslash_top_folder_and_content_mismatch(self):
        path = self.folder()
        tree = self.package(path)
        main = path / "ModAVD-0.33.0-win-x64.zip"
        self.write_zip(main, tree, "")
        release.verify_zip(SimpleNamespace(output=str(path)))
        # A versioned top-level folder would make every MirrorChyan diff a full package.
        for bad in [lambda: self.write_zip(main, tree, "", raw_names={"sdk/emulator/data": "sdk\\emulator/data"}),
                    lambda: self.write_zip(main, tree, "", {"sdk/emulator/data": b"wrong"}),
                    lambda: self.write_zip(main, tree, "ModAVD-0.33.0-win-x64/")]:
            bad()
            with self.assertRaises(ValueError):
                release.verify_zip(SimpleNamespace(output=str(path)))

    def test_manifest_covers_every_file_exactly(self):
        path = self.folder()
        tree = self.package(path)
        manifest = checks.check_manifest(tree)
        self.assertEqual((manifest["version"], manifest["platform"]), ("v0.33.0", "win-x64"))
        (tree / "extra").write_bytes(b"new")
        with self.assertRaises(ValueError):
            checks.check_manifest(tree)
        (tree / "extra").unlink()
        (tree / "sdk/emulator/data").write_bytes(b"changed")
        with self.assertRaises(ValueError):
            checks.check_manifest(tree)

    def test_manifest_build_is_not_the_version(self):
        """Since 0.41 the version counts on its own: 0.41.0 can be mas-40. The build only has to be a build number."""
        path = self.folder()
        tree = self.package(path)
        manifest = tree / release.MANIFEST
        for version, build in [("0.41.0", "mas40"), ("0.42.0", "mas41"), ("0.41.1", "mas40"), ("0.40.0", "mas40")]:
            release.write_manifest(tree, {"build": build, "version": version, "upstream": "37.2.10", "min_automas": "5.7.0"})
            data = checks.check_manifest(tree)
            self.assertEqual((data["version"], data["build"]), ("v" + version, int(build[3:])))
        good = release.load(manifest)
        for field, value in [("build", 0), ("build", "40"), ("build", True), ("build", None), ("version", "0.41.0"),
                             ("version", "v0.41")]:
            release.save(manifest, {**good, field: value})
            with self.assertRaises(ValueError):
                checks.check_manifest(tree)

    def test_components_are_pinned_and_complete(self):
        path = self.folder()
        launcher = path / "de.jrpie.android.launcher_57.apk"
        launcher.write_bytes(b"launcher")
        ime = path / release.IME_APK
        ime.write_bytes(b"ime")
        pins = path / "components.SHA256SUMS.txt"
        line = lambda f, name: release.sha(f) + "  components/" + name + "\n"
        pins.write_text(line(launcher, launcher.name), encoding="utf-8")
        without = SimpleNamespace(apk=str(launcher), ime_apk="")
        with_ime = SimpleNamespace(apk=str(launcher), ime_apk=str(ime))
        self.assertEqual(list(release.pinned_components(without, pins)), ["components/" + launcher.name])
        # shipping an APK nobody pinned is refused
        with self.assertRaises(ValueError):
            release.pinned_components(with_ime, pins)
        pins.write_text(line(launcher, launcher.name) + line(ime, release.IME_APK), encoding="utf-8")
        self.assertEqual(sorted(release.pinned_components(with_ime, pins)), sorted(["components/" + launcher.name, "components/" + release.IME_APK]))
        # once pinned it has to come every time (else the incremental update deletes it), with exactly that content
        with self.assertRaises(ValueError):
            release.pinned_components(without, pins)
        ime.write_bytes(b"another build")
        with self.assertRaises(ValueError):
            release.pinned_components(with_ime, pins)
        ime.write_bytes(b"ime")
        # the desktop APK cannot take the IME's name, not even in another case (NTFS ignores case)
        only_ime = path / "only-ime.SHA256SUMS.txt"
        only_ime.write_text(line(ime, release.IME_APK), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "collides"):
            release.pinned_components(SimpleNamespace(apk=str(ime), ime_apk=str(ime)), only_ime)
        upper = path / "MODAVD-IME.APK"
        upper.write_bytes(b"ime")
        with self.assertRaisesRegex(ValueError, "collides"):
            release.pinned_components(SimpleNamespace(apk=str(upper), ime_apk=str(ime)), only_ime)
        # pin lines are distinct components/<name>, case-insensitively
        bad = path / "bad.SHA256SUMS.txt"
        bad.write_text(line(ime, release.IME_APK) + line(upper, "MODAVD-IME.APK"), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "distinct"):
            release.pinned_components(with_ime, bad)
        # the extras DLL is pinned like the APKs, at exactly extras/modavd_extras.dll
        dll = path / "modavd_extras.dll"
        dll.write_bytes(b"dll")
        with_extras = SimpleNamespace(apk=str(launcher), ime_apk=str(ime), extras_dll=str(dll))
        extras_line = release.sha(dll) + "  " + release.EXTRAS_DLL + "\n"
        with self.assertRaisesRegex(ValueError, "extras/modavd_extras.dll is not pinned"):
            release.pinned_components(with_extras, pins)
        pins.write_text(line(launcher, launcher.name) + line(ime, release.IME_APK) + extras_line, encoding="utf-8")
        self.assertEqual(sorted(release.pinned_components(with_extras, pins)),
                         sorted(["components/" + launcher.name, "components/" + release.IME_APK, release.EXTRAS_DLL]))
        # pinned, so it has to come every time; and the file has to be there
        with self.assertRaisesRegex(ValueError, r"missing from this release \(pass -ImeApk / -Apk / -ExtrasDll\).*extras/modavd_extras.dll"):
            release.pinned_components(with_ime, pins)
        with self.assertRaises(FileNotFoundError):
            release.pinned_components(SimpleNamespace(**{**vars(with_extras), "extras_dll": str(path / "gone.dll")}), pins)
        # another build (a local one: another MSVC gives other bytes) is refused
        dll.write_bytes(b"local build")
        with self.assertRaisesRegex(ValueError, "extras/modavd_extras.dll differs from its pinned SHA-256"):
            release.pinned_components(with_extras, pins)
        # no other path outside components/ can be pinned
        for other in ["extras/other.dll", "modavd_extras.dll", "extras/x/modavd_extras.dll", "EXTRAS/MODAVD_EXTRAS.DLL"]:
            bad.write_text(line(launcher, launcher.name) + release.sha(dll) + "  " + other + "\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "distinct components/<file name> or extras/modavd_extras.dll"):
                release.pinned_components(with_extras, bad)

    def test_draft_takes_tagged_components_only(self):
        """-DraftRelease (--require-tags) refuses a package whose manager or extras library is not tagged yet."""
        tagged = {"manager": {"version": "0.8.0", "tagged": True}, "ime": None, "extras": {"version": "0.1.2", "tagged": True},
                  "modavd": {"commit": "0" * 40}}
        pending = {**tagged, "manager": {"version": "0.8.0", "tagged": False}, "extras": {"version": "0.1.3", "tagged": False}}
        draft = SimpleNamespace(require_tags=True)
        release.check_tagged(draft, tagged)
        # a snapshot written before the key existed was at its tags
        release.check_tagged(draft, {**tagged, "manager": {"version": "0.8.0"}})
        release.check_tagged(SimpleNamespace(require_tags=False), pending)
        release.check_tagged(SimpleNamespace(), pending)
        with self.assertRaisesRegex(ValueError, r"Not tagged yet: manager v0\.8\.0, extras v0\.1\.3"):
            release.check_tagged(draft, pending)
        with self.assertRaisesRegex(ValueError, r"Not tagged yet: extras v0\.1\.3;"):
            release.check_tagged(draft, {**pending, "manager": tagged["manager"]})

    def test_texts_extended_and_every_package_file_named(self):
        tree = self.folder()
        sep = "=" * 59
        def entry(lib, files, body="body"):
            return "\n".join([sep, "Notices for file(s):", *files, f"The notices is included for the library: {lib}", "", sep, body, "", ""])
        # the reviewed NOTICE calls the icon fan art, in Part 1 and in its own entry (ICON_NOTICE_EDITS)
        icon_ref, _ = release.ICON_NOTICE_EDITS[0]
        icon_body = "The icon (from qemu as changed by\n" + release.ICON_NOTICE_EDITS[2][0] + "\nNot licensed."
        # the reviewed texts still name the batch files and scripts the package no longer ships (NOTICE_GONE_FILES)
        gone = ["tools/fetch-components.ps1", "tools/collect-info.ps1", "补全组件.bat", "收集问题信息.bat"]
        icon_files, script_files = ["./emulator.exe"], [*(f"(package root) {n}" for n in gone), "(package root) 说明.txt",
                                                         "(package root) LICENSES/NOTICE.txt", "(package root) LICENSES/NOTICE.csv"]
        # and its scripts entry speaks of batch files (SCRIPT_NOTICE_EDITS)
        script_body = "".join(old for old, _ in release.SCRIPT_NOTICE_EDITS)
        notice = ("header: " + icon_ref + "\n\n" + entry(release.ICON_ENTRY_OLD, icon_files, icon_body) +
                  entry("ModAVD scripts and documents", script_files, script_body) + "#" * 79 + "\n# PART 3. Full license texts\n")
        (tree / "LICENSES").mkdir()
        (tree / "LICENSES/NOTICE.txt").write_text(notice, encoding="utf-8")
        rows = [["External Library Name", "Link to License", "License Name", "Binaries using the license", "URL", "ModAVD note"],
                [release.ICON_ENTRY_OLD, "", "Not licensed (see text)", " ".join(icon_files), "", "a, quoted note"],
                ["ModAVD scripts and documents", "", release.SCRIPT_CSV_LICENSE[0],
                 " ".join(script_files), "", ""]]
        (tree / "LICENSES/NOTICE.csv").write_text(release.render_csv(rows), encoding="utf-8")
        (tree / "LICENSES/Apache-2.0.txt").write_text("license text", encoding="utf-8")
        # the reviewed 说明.txt has each passage README_EDITS rewrites
        step, trouble, licence, icon, upgrade = (old for old, _ in release.README_EDITS)
        readme = ("安装\n\n1. 解压。\n\n" + step + "\n3. 新建实例。\n\n- 升级：整个覆盖解压。\n" + upgrade +
                  "\n====================\n出问题怎么办\n====================\n\n" +
                  trouble + "\n====================\n许可证\n====================\n\n" + licence + "AGPL-3.0-or-later 发布。\n" + icon)
        (tree / "说明.txt").write_text(readme, encoding="utf-8")
        shipped = [*release.MODAVD_FILES.values(), "tools/modavd-bridge.ps1", release.EXTRAS_DLL,
                   "sdk/emulator/emulator.exe", *(f"sdk/emulator/resources/modavd/{n}" for n in
                                                  ["libmodavd_bridge.so", "libmodavd_gate.so", "modavd_bridge.rc"])]
        for rel in shipped:
            (tree / rel).parent.mkdir(parents=True, exist_ok=True)
            (tree / rel).write_bytes(b"x")
        # the reviewed NOTICE names files this package does not have
        with self.assertRaisesRegex(ValueError, "does not have: tools/collect-info.ps1, tools/fetch-components.ps1, 收集问题信息.bat, 补全组件.bat$"):
            release.check_notice_coverage(tree)
        for rel in gone:
            (tree / rel).write_bytes(b"x")
        with self.assertRaisesRegex(ValueError, "no entry for: .*ModAVD.exe.*libmodavd_bridge.so.*tools/modavd.exe") as caught:
            release.check_notice_coverage(tree)
        self.assertIn("extras/modavd_extras.dll", str(caught.exception))
        for rel in gone:
            (tree / rel).unlink()
        release.extend_texts(tree, "go1.26.5")
        release.check_notice_coverage(tree)
        text = (tree / "LICENSES/NOTICE.txt").read_text(encoding="utf-8")
        self.assertIn("(package root) ModAVD.exe\nThe notices is included for the library: ModAVD program icon\n", text)
        self.assertIn('header: see "ModAVD program icon" in Part 2.\n', text)
        self.assertIn("as changed by\nModAVD) is ModAVD's own icon.\n\nNot licensed.", text)
        for old in ["fan art", "Eyjafjalla", "Hypergryph", "batch files", ".bat files"]:
            self.assertNotIn(old, text)
        for _, new in release.SCRIPT_NOTICE_EDITS:
            self.assertEqual(text.count(new), 1)
        self.assertIn("Go go1.26.5 (runtime and standard library)", text)
        self.assertLess(text.index("ModAVD arm64 execution bridge"), text.index("# PART 3."))
        # the extras DLL: its own entry, LGPL-3.0-or-later with both texts it needs
        extras = text[text.index("(package root) extras/modavd_extras.dll\n"):text.index("# PART 3.")]
        for part in ["ModAVDExtras capture library", "GNU Lesser General Public License", "LICENSES\\LGPL-3.0.txt",
                     "LICENSES\\GPL-3.0.txt", "https://github.com/ModAVD/ModAVDExtras", "ModAVDExtras in modavd-src.tar.xz"]:
            self.assertIn(part, extras)
        self.assertNotIn("{{", text)
        csv_rows = list(release.csv.reader(io.StringIO((tree / "LICENSES/NOTICE.csv").read_text(encoding="utf-8"))))
        self.assertEqual([csv_rows[1][0], csv_rows[1][5]], [release.ICON_ENTRY, release.ICON_CSV_NOTE])
        self.assertEqual(csv_rows[2][2], release.SCRIPT_CSV_LICENSE[1])
        self.assertTrue(csv_rows[1][3].endswith("(package root) ModAVD.exe"))
        self.assertEqual(len(csv_rows), 7)
        self.assertEqual(csv_rows[6][2:4], ["LGPL-3.0-or-later", "(package root) extras/modavd_extras.dll"])
        # the files that are gone are out of the entry, in both texts; the rest of it stays in order
        for name in gone:
            self.assertNotIn(name, text)
            self.assertNotIn(name, csv_rows[2][3])
        self.assertIn("Notices for file(s):\n(package root) 说明.txt\n(package root) LICENSES/NOTICE.txt\n", text)
        self.assertIn("(package root) LICENSES/NOTICE.csv\n(package root) tools/modavd-bridge.ps1\nThe notices", text)
        self.assertEqual(csv_rows[2][3], "(package root) 说明.txt (package root) LICENSES/NOTICE.txt "
                                         "(package root) LICENSES/NOTICE.csv (package root) tools/modavd-bridge.ps1")
        # 说明.txt: the batch files are gone from it, the manager and the CLI take their place
        shown = (tree / "说明.txt").read_text(encoding="utf-8")
        self.assertIn("管理器和命令行", shown)
        self.assertNotIn(".bat", shown)
        self.assertNotIn("同人", shown)
        for _, new in release.README_EDITS:
            self.assertEqual(shown.count(new), 1)
        self.assertIn("tools\\modavd.exe components fetch", shown)
        self.assertIn("tools\\modavd.exe collect-info", shown)
        self.assertLess(shown.index("1. 解压。"), shown.index("2. 双击 ModAVD.exe"))
        self.assertLess(shown.index("2. 双击 ModAVD.exe"), shown.index("3. 新建实例。"))
        # each passage has to be there exactly once: refuse rather than guess
        for old, _ in release.README_EDITS:
            for broken in [readme.replace(old, ""), readme + old]:
                (tree / "LICENSES/NOTICE.txt").write_text(notice, encoding="utf-8")
                (tree / "LICENSES/NOTICE.csv").write_text(release.render_csv(rows), encoding="utf-8")
                (tree / "说明.txt").write_text(broken, encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "说明.txt: the passage " + re.escape(repr(old.splitlines()[0]))):
                    release.extend_texts(tree, "go1.26.5")
        # an icon passage missing or twice in NOTICE.txt, or the icon entry missing from NOTICE.csv: refuse
        (tree / "LICENSES/NOTICE.csv").write_text(release.render_csv(rows), encoding="utf-8")
        (tree / "说明.txt").write_text(readme, encoding="utf-8")
        for old, _ in release.ICON_NOTICE_EDITS:
            for broken in [notice.replace(old, "\n"), notice + old]:
                (tree / "LICENSES/NOTICE.txt").write_text(broken, encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "NOTICE.txt: the passage " + re.escape(repr(old.strip().splitlines()[0]))):
                    release.extend_texts(tree, "go1.26.5")
        (tree / "LICENSES/NOTICE.txt").write_text(notice, encoding="utf-8")
        rows_icon = [list(r) for r in rows]
        rows_icon[1][0] = release.ICON_ENTRY
        (tree / "LICENSES/NOTICE.csv").write_text(release.render_csv(rows_icon), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "NOTICE.csv: entry 'ModAVD program icon \\(fan art\\)'"):
            release.extend_texts(tree, "go1.26.5")
        # a file to take out that the reviewed texts do not name (any more): refuse rather than guess, in either text
        (tree / "LICENSES/NOTICE.csv").write_text(release.render_csv(rows), encoding="utf-8")
        (tree / "LICENSES/NOTICE.txt").write_text(notice.replace("(package root) tools/collect-info.ps1\n", ""), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "NOTICE.txt.*collect-info.ps1"):
            release.extend_texts(tree, "go1.26.5")
        (tree / "LICENSES/NOTICE.txt").write_text(notice, encoding="utf-8")
        rows_gone = [list(r) for r in rows]
        rows_gone[2][3] = rows_gone[2][3].replace(" (package root) tools/collect-info.ps1", "")
        (tree / "LICENSES/NOTICE.csv").write_text(release.render_csv(rows_gone), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "NOTICE.csv.*collect-info.ps1"):
            release.extend_texts(tree, "go1.26.5")
        (tree / "LICENSES/NOTICE.txt").write_text(notice, encoding="utf-8")
        (tree / "LICENSES/NOTICE.csv").write_text(release.render_csv(rows), encoding="utf-8")
        # an entry the texts were reviewed with is gone: refuse rather than guess
        (tree / "说明.txt").write_text("安装\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "说明.txt"):
            release.extend_texts(tree, "go1.26.5")
        (tree / "说明.txt").write_text("====================\n出问题怎么办\n", encoding="utf-8")
        (tree / "LICENSES/NOTICE.txt").write_text(notice.replace("ModAVD scripts and documents", "Scripts"), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "ModAVD scripts and documents"):
            release.extend_texts(tree, "go1.26.5")
        # from good texts again: a file nobody listed
        for rel, data in [("LICENSES/NOTICE.txt", notice), ("LICENSES/NOTICE.csv", release.render_csv(rows)), ("说明.txt", readme)]:
            (tree / rel).write_text(data, encoding="utf-8")
        release.extend_texts(tree, "go1.26.5")
        release.check_notice_coverage(tree)
        (tree / "tools/new.ps1").write_bytes(b"x")
        with self.assertRaisesRegex(ValueError, "no entry for: tools/new.ps1$"):
            release.check_notice_coverage(tree)
        # a listed file the package does not have
        (tree / "tools/new.ps1").unlink()
        extended = (tree / "LICENSES/NOTICE.txt").read_text(encoding="utf-8")
        (tree / "LICENSES/NOTICE.txt").write_text(extended.replace("(package root) 说明.txt\n", "(package root) 说明.txt\n(package root) tools/old.ps1\n"),
                                                  encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "does not have: tools/old.ps1$"):
            release.check_notice_coverage(tree)

    def test_license_package_needs_agpl_full_text(self):
        package = self.folder()
        with self.assertRaisesRegex(FileNotFoundError, "AGPL-3.0.txt"):
            release.check_agpl(package)
        text = (release.REPO / "package/LICENSE").read_bytes().replace(b"\r\n", b"\n")
        self.assertEqual(hashlib.sha256(text).hexdigest(), release.AGPL_SHA256)
        (package / "LICENSES").mkdir()
        # the reviewed P3 texts are CRLF: the hash is taken with LF line endings
        (package / "LICENSES/AGPL-3.0.txt").write_bytes(text.replace(b"\n", b"\r\n"))
        release.check_agpl(package)
        (package / "LICENSES/AGPL-3.0.txt").write_bytes(text + b"\nextra")
        with self.assertRaisesRegex(ValueError, "not the AGPL-3.0 full text"):
            release.check_agpl(package)

    def test_license_package_needs_lgpl_and_gpl_texts_for_extras(self):
        """The extras DLL is LGPL-3.0-or-later: its NOTICE entry points to LICENSES/LGPL-3.0.txt and LICENSES/GPL-3.0.txt,
        so the license package must have both, with the pinned contents (any line endings)."""
        package = self.folder()
        self.assertEqual(sorted(release.LGPL_TEXTS), ["GPL-3.0.txt", "LGPL-3.0.txt"])
        with self.assertRaisesRegex(FileNotFoundError, "LGPL-3.0.txt is missing"):
            release.check_lgpl(package)
        (package / "LICENSES").mkdir()
        texts = {"LGPL-3.0.txt": b"lesser\n", "GPL-3.0.txt": b"general\n"}
        with mock.patch.dict(release.LGPL_TEXTS, {n: hashlib.sha256(t).hexdigest() for n, t in texts.items()}):
            (package / "LICENSES/LGPL-3.0.txt").write_bytes(texts["LGPL-3.0.txt"].replace(b"\n", b"\r\n"))
            with self.assertRaisesRegex(FileNotFoundError, r"[\\/]GPL-3\.0\.txt is missing"):
                release.check_lgpl(package)
            (package / "LICENSES/GPL-3.0.txt").write_bytes(texts["GPL-3.0.txt"])
            release.check_lgpl(package)
            (package / "LICENSES/GPL-3.0.txt").write_bytes(b"GPL-2.0\n")
            with self.assertRaisesRegex(ValueError, "not the GPL-3.0 full text"):
                release.check_lgpl(package)
            (package / "LICENSES/GPL-3.0.txt").write_bytes(texts["GPL-3.0.txt"])
            (package / "LICENSES/LGPL-3.0.txt").write_bytes(b"LGPL-2.1\n")
            with self.assertRaisesRegex(ValueError, "not the LGPL-3.0 full text"):
                release.check_lgpl(package)

    @staticmethod
    def commit_files(repo, files, tag=None):
        for rel, data in files.items():
            (repo / rel).parent.mkdir(parents=True, exist_ok=True)
            (repo / rel).write_bytes(data)
        if not (repo / ".git").exists():
            release.git(repo, "init", "-q")
        release.git(repo, "add", "-A")
        release.git(repo, "-c", "user.name=t", "-c", "user.email=t@example.com", "commit", "-q", "-m", "x")
        if tag:
            release.git(repo, "tag", tag)

    @staticmethod
    def sums_line(name, data):
        return f"{hashlib.sha256(data).hexdigest()}  {name}\n"

    def public_repos(self, root):
        """Stand-ins for the public checkouts: ModAVDManager with tag v0.2.0 and its release checksums, ModAVDHostIME
        with tag v1.0.0, and a ModAVD checkout with package/ and bridge/. Returns the arguments public_sources reads."""
        manager = root / "ModAVDManager-0.2.0-win-x64.zip"
        programs = {"ModAVD.exe": b"gui", "tools/modavd.exe": b"cli"}
        with zipfile.ZipFile(manager, "w") as z:
            for name, data in programs.items():
                z.writestr(name, data)
        listed = {**programs, manager.name: manager.read_bytes()}
        self.commit_files(root / "mgr", {"go.mod": b"module modavd\n\ngo 1.26\n\ntoolchain go1.26.5\n", "main.go": b"package main\n",
                                         "main_test.go": b"package main\n",
                                         "release/0.2.0.SHA256SUMS.txt": "".join(self.sums_line(n, d) for n, d in sorted(listed.items())).encode()},
                          "v0.2.0")
        apk = root / "modavd-ime.apk"
        apk.write_bytes(b"apk")
        self.commit_files(root / "ime", {"ime/AndroidManifest.xml": b"<manifest/>\n",
                                         "release/1.0.0.SHA256SUMS.txt": self.sums_line(release.IME_APK, b"apk").encode()}, "v1.0.0")
        # ModAVDExtras at tag v<EXTRAS_VERSION>, whose release checksums list the DLL (in the order its workflow writes)
        dll = root / "modavd_extras.dll"
        dll.write_bytes(b"dll")
        version = release.EXTRAS_VERSION
        self.commit_files(root / "extras", {"CMakeLists.txt": f"project(ModAVDExtras VERSION {version} LANGUAGES C)\n".encode(),
                                            "LICENSE": b"lgpl\n", "COPYING": b"gpl\n", "src/core.c": b"int x;\n",
                                            f"release/{version}.SHA256SUMS.txt": (self.sums_line("external_renderer_ipc.dll", b"shim") +
                                                                                  self.sums_line("modavd_extras.dll", b"dll")).encode()},
                          "v" + version)
        self.commit_files(root / "pub", {".gitattributes": b"package/** -text\nbridge/** -text\n", "README.md": b"# ModAVD\n",
                                         "patches/BASE.md": b"x\n", "bridge/nbshim/modavd_bridge.rc": b"service modavd\r\n",
                                         "bridge/nbshim/jni/native_bridge_shim.cc": b"int x;\n",
                                         # a batch file left in package/ is not shipped
                                         "package/补全组件.bat": b"bat\r\n",
                                         **{f"package/{n}": f"{n}\r\n".encode() for n in release.PACKAGE_TOOLS}})
        return SimpleNamespace(manager=str(manager), manager_repo=str(root / "mgr"), ime_apk=str(apk), ime_repo=str(root / "ime"),
                               extras_dll=str(dll), extras_repo=str(root / "extras"), repository=str(root / "pub"))

    def test_public_sources_checked_against_release_checksums(self):
        root = self.folder()
        args = self.public_repos(root)
        own = release.public_sources(args)
        self.assertEqual((own["manager"]["version"], own["manager"]["tag"], own["manager"]["go"]), ("0.2.0", "v0.2.0", "go1.26.5"))
        self.assertEqual(own["ime"]["tag"], "v1.0.0")
        self.assertEqual(own["modavd"]["commit"], release.git(root / "pub", "rev-parse", "HEAD"))
        self.assertIsNone(release.public_sources(SimpleNamespace(**{**vars(args), "ime_apk": None}))["ime"])
        dest = root / "tree"
        release.copy_public_files(own, args.manager, dest)
        self.assertEqual((dest / "tools/modavd.exe").read_bytes(), b"cli")
        self.assertEqual((dest / "tools/modavd-bridge.ps1").read_bytes(), b"modavd-bridge.ps1\r\n")
        self.assertEqual(sorted(p.relative_to(dest).as_posix() for p in dest.rglob("*") if p.is_file()),
                         ["ModAVD.exe", "tools/modavd-bridge.ps1", "tools/modavd.exe"])
        # a rebuilt ZIP under the published name
        other = root / "other" / Path(args.manager).name
        other.parent.mkdir()
        with zipfile.ZipFile(other, "w") as z:
            z.writestr("ModAVD.exe", b"gui")
            z.writestr("tools/modavd.exe", b"cli2")
        # a release whose checksums list this ZIP but another tools/modavd.exe
        v3 = root / "v3" / "ModAVDManager-0.3.0-win-x64.zip"
        v3.parent.mkdir()
        v3.write_bytes(other.read_bytes())
        self.commit_files(root / "mgr", {"release/0.3.0.SHA256SUMS.txt": (self.sums_line(v3.name, v3.read_bytes()) +
                                                                          self.sums_line("ModAVD.exe", b"gui") +
                                                                          self.sums_line("tools/modavd.exe", b"cli")).encode()}, "v0.3.0")
        unlisted = root / "unlisted.apk"
        unlisted.write_bytes(b"apk2")
        for change, message in [({"manager": str(other)}, "not the published one"),
                                ({"manager": str(v3)}, "tools/modavd.exe.*not the published one"),
                                ({"manager": str(root / "ModAVDManager.zip")}, "ModAVDManager-<version>-win-x64.zip"),
                                ({"manager_repo": str(root / "ime")}, "no tag v0.2.0"),
                                ({"ime_apk": str(unlisted)}, "No tag"),
                                ({"repository": str(root / "mgr")}, "ModAVD checkout")]:
            with self.assertRaisesRegex(ValueError, message):
                release.public_sources(SimpleNamespace(**{**vars(args), **change}))
        (root / "pub/package/modavd-bridge.ps1").write_bytes(b"changed\r\n")
        with self.assertRaisesRegex(ValueError, "uncommitted changes under package/"):
            release.public_sources(args)

    def test_untagged_components_come_from_the_release_branch(self):
        """Before a manager or extras version is tagged, it is taken at the head of upstream's release branch, checked
        against the checksums committed there; a tag, once it exists, wins."""
        root = self.folder()
        args = self.public_repos(root)
        mgr, extras, branch = root / "mgr", root / "extras", "release/0.40"
        with_branch = SimpleNamespace(**{**vars(args), "release_branch": branch})
        # everything tagged: the branch does not matter (it does not even exist)
        own = release.public_sources(with_branch)
        self.assertEqual((own["manager"]["tagged"], own["extras"]["tagged"]), (True, True))
        self.assertEqual(own, release.public_sources(args))
        # manager 0.4.0 prepared on the release branch, not tagged
        zip4 = root / "next" / "ModAVDManager-0.4.0-win-x64.zip"
        zip4.parent.mkdir()
        with zipfile.ZipFile(zip4, "w") as z:
            z.writestr("ModAVD.exe", b"gui4")
            z.writestr("tools/modavd.exe", b"cli4")
        sums = (self.sums_line(zip4.name, zip4.read_bytes()) + self.sums_line("ModAVD.exe", b"gui4") +
                self.sums_line("tools/modavd.exe", b"cli4")).encode()
        self.commit_files(mgr, {"release/0.4.0.SHA256SUMS.txt": sums, "internal/buildinfo/buildinfo.go": b'package buildinfo\n\nvar Version = "0.4.0"\n'})
        head = release.git(mgr, "rev-parse", "HEAD")
        next_args = SimpleNamespace(**{**vars(with_branch), "manager": str(zip4)})
        # without the branch it is simply not released; with a branch that was not fetched, the message names both
        with self.assertRaisesRegex(ValueError, "has no tag v0.4.0"):
            release.public_sources(SimpleNamespace(**{**vars(args), "manager": str(zip4)}))
        with self.assertRaisesRegex(ValueError, "neither tag v0.4.0 nor refs/remotes/upstream/release/0.40"):
            release.public_sources(next_args)
        release.git(mgr, "update-ref", "refs/remotes/upstream/" + branch, head)
        got = release.public_sources(next_args)["manager"]
        self.assertEqual({k: got[k] for k in ("version", "tag", "commit", "tagged", "go")},
                         {"version": "0.4.0", "tag": "v0.4.0", "commit": head, "tagged": False, "go": "go1.26.5"})
        # the run that makes the draft (--require-tags; preflight and assemble both go through checked_sources) refuses it
        self.assertEqual(release.checked_sources(next_args)["manager"]["commit"], head)
        with self.assertRaisesRegex(ValueError, r"Not tagged yet: manager v0\.4\.0;"):
            release.checked_sources(SimpleNamespace(**{**vars(next_args), "require_tags": True}))
        release.checked_sources(SimpleNamespace(**{**vars(with_branch), "require_tags": True}))
        described = release.describe_sources(release.public_sources(next_args))
        self.assertIn(f"ModAVDManager v0.4.0 (NOT TAGGED YET: release branch head {head[:12]})", described)
        self.assertIn(f"ModAVDExtras v{release.EXTRAS_VERSION},", described)
        # another build of it is refused as at a tag
        other = root / "other4" / zip4.name
        other.parent.mkdir()
        with zipfile.ZipFile(other, "w") as z:
            z.writestr("ModAVD.exe", b"gui4")
            z.writestr("tools/modavd.exe", b"local")
        with self.assertRaisesRegex(ValueError, "not the published one"):
            release.public_sources(SimpleNamespace(**{**vars(next_args), "manager": str(other)}))
        # the branch head must already be that version: checksums left from a tagged version do not make it one
        self.commit_files(mgr, {"internal/buildinfo/buildinfo.go": b'package buildinfo\n\nvar Version = "0.5.0"\n'})
        release.git(mgr, "update-ref", "refs/remotes/upstream/" + branch, release.git(mgr, "rev-parse", "HEAD"))
        with self.assertRaisesRegex(ValueError, r"buildinfo\.go at release/0\.40 \([0-9a-f]{12}\) says \['0\.5\.0'\], not 0\.4\.0"):
            release.public_sources(next_args)
        # once tagged, the tag is what counts, wherever the branch went since
        release.git(mgr, "tag", "v0.4.0", head)
        got = release.public_sources(next_args)["manager"]
        self.assertEqual((got["commit"], got["tagged"]), (head, True))
        # the checksums are read at that commit: what a later commit (the checkout's HEAD) lists does not count
        self.commit_files(mgr, {"release/0.4.0.SHA256SUMS.txt": self.sums_line(zip4.name, b"other").encode()})
        self.assertEqual(release.public_sources(next_args)["manager"]["commit"], head)
        # the extras library the same way: a branch head at EXTRAS_VERSION whose checksums list the DLL
        release.git(extras, "tag", "-d", "v" + release.EXTRAS_VERSION)
        with self.assertRaisesRegex(ValueError, "neither tag"):
            release.public_sources(with_branch)
        ehead = release.git(extras, "rev-parse", "HEAD")
        release.git(extras, "update-ref", "refs/remotes/upstream/" + branch, ehead)
        got = release.public_sources(with_branch)["extras"]
        self.assertEqual((got["tag"], got["commit"], got["tagged"]), ("v" + release.EXTRAS_VERSION, ehead, False))
        self.commit_files(extras, {"CMakeLists.txt": b"project(ModAVDExtras VERSION 9.9.9 LANGUAGES C)\n"})
        release.git(extras, "update-ref", "refs/remotes/upstream/" + branch, release.git(extras, "rev-parse", "HEAD"))
        with self.assertRaisesRegex(ValueError, r"CMakeLists\.txt at release/0\.40 .* says \['9\.9\.9'\], not"):
            release.public_sources(with_branch)

    def test_extras_dll_checked_against_its_tag(self):
        """The extras DLL of a release is the one tag v<EXTRAS_VERSION> of ModAVDExtras lists."""
        root = self.folder()
        args = self.public_repos(root)
        version, tag, extras = release.EXTRAS_VERSION, "v" + release.EXTRAS_VERSION, root / "extras"
        own = release.public_sources(args)
        self.assertEqual({k: own["extras"][k] for k in ("version", "tag", "sha256")},
                         {"version": version, "tag": tag, "sha256": hashlib.sha256(b"dll").hexdigest()})
        self.assertEqual(own["extras"]["commit"], release.git(extras, "rev-parse", tag))
        self.assertTrue(own["extras"]["tagged"])
        self.assertIn(f"ModAVDExtras {tag},", release.describe_sources(own))
        self.assertIsNone(release.public_sources(SimpleNamespace(**{**vars(args), "extras_dll": None}))["extras"])
        # a local build is not the published DLL (another MSVC gives other bytes)
        local = root / "local/modavd_extras.dll"
        local.parent.mkdir()
        local.write_bytes(b"local build")
        nosums = root / "nosums"
        self.commit_files(nosums, {"CMakeLists.txt": f"project(ModAVDExtras VERSION {version} LANGUAGES C)\n".encode()}, tag)
        for change, message in [({"extras_dll": str(local)}, f"not the published modavd_extras.dll: SHA-256 {release.sha(local)}"),
                                ({"extras_repo": None}, "needs the ModAVDExtras checkout"),
                                # not released yet, or the tags not fetched
                                ({"extras_repo": str(root / "ime")}, f"has no tag {re.escape(tag)}"),
                                ({"extras_repo": str(nosums)}, f"no release/{re.escape(version)}.SHA256SUMS.txt at {re.escape(tag)}")]:
            with self.assertRaisesRegex(ValueError, message):
                release.public_sources(SimpleNamespace(**{**vars(args), **change}))
        # the release takes the tag, not HEAD
        self.commit_files(extras, {"src/core.c": b"int y;\n"})
        self.assertEqual(release.public_sources(args)["extras"]["commit"], own["extras"]["commit"])

    def test_own_sources_archive_has_license_notice_and_public_repos(self):
        """modavd-src has LICENSE and NOTICE (from this repository's HEAD) at its root, then each public repository as
        published under its own folder (only bridge/ of ModAVD), and comes out byte for byte the same twice."""
        path = self.folder()
        own = release.public_sources(self.public_repos(path))
        results = []
        for n in "ab":
            (path / n / "source").mkdir(parents=True)
            try:
                results.append(release.own_sources(path / n, path / n / "source", own))
            except ValueError as e:
                if "Uncommitted changes" not in str(e):
                    raise
                self.skipTest("LICENSE / NOTICE of modavd-src have uncommitted changes; commit, then run again")
        a, b = results
        self.assertEqual(a, b)
        with tarfile.open(path / "a/source" / release.OWN_SOURCES_NAME) as tar:
            names = tar.getnames()
            license = tar.extractfile("modavd-src/LICENSE").read()
            notice = tar.extractfile("modavd-src/NOTICE").read().decode("utf-8")
            rc = tar.extractfile("modavd-src/ModAVD/bridge/nbshim/modavd_bridge.rc").read()
        self.assertEqual(hashlib.sha256(license).hexdigest(), release.AGPL_SHA256)
        for text in ["AGPL-3.0-or-later", "ModAVDManager/internal/avd/testdata/instances_ref.py", "native_bridge.h", "LLVM-exception",
                     "https://github.com/ModAVD/ModAVDManager", "https://github.com/ModAVD/ModAVDHostIME",
                     "https://github.com/ModAVD/ModAVD/tree/main/bridge", "https://github.com/ModAVD/ModAVD/tree/main/package",
                     # ModAVDExtras is the exception to the AGPL: LGPL, with its own texts
                     "https://github.com/ModAVD/ModAVDExtras", "LGPL-3.0-or-later", "ModAVDExtras/LICENSE", "ModAVDExtras/COPYING"]:
            self.assertIn(text, notice)
        self.assertEqual(names[:2], ["modavd-src/LICENSE", "modavd-src/NOTICE"])
        for name in ["modavd-src/ModAVDManager/go.mod", "modavd-src/ModAVDManager/main_test.go",
                     "modavd-src/ModAVDHostIME/ime/AndroidManifest.xml", "modavd-src/ModAVD/bridge/nbshim/jni/native_bridge_shim.cc",
                     "modavd-src/ModAVDExtras/CMakeLists.txt", "modavd-src/ModAVDExtras/LICENSE", "modavd-src/ModAVDExtras/COPYING"]:
            self.assertIn(name, names)
        self.assertFalse([n for n in names if n.startswith(("modavd-src/ModAVD/package", "modavd-src/ModAVD/README"))])
        self.assertEqual(rc, b"service modavd\r\n")
        text = release.own_sources_text(own)
        for part in ["ModAVDManager v0.2.0", "ModAVDManager-0.2.0-win-x64.zip", "Go go1.26.5", "ModAVDHostIME v1.0.0",
                     own["modavd"]["commit"][:12], f"ModAVDExtras v{release.EXTRAS_VERSION}（提交 {own['extras']['commit'][:12]}），即它的 Release",
                     "LGPL-3.0-or-later"]:
            self.assertIn(part, text)
        self.assertNotIn("{", text)
        self.assertNotIn("ModAVDHostIME", release.own_sources_text({**own, "ime": None}))
        self.assertNotIn("ModAVDExtras", release.own_sources_text({**own, "extras": None}))
        # an older snapshot without the key
        self.assertNotIn("ModAVDExtras", release.own_sources_text({k: v for k, v in own.items() if k != "extras"}))
        # tagged or not yet, the text is the same: the package does not change when the tag is made
        pending = {**own, "manager": {**own["manager"], "tagged": False}, "extras": {**own["extras"], "tagged": False}}
        self.assertEqual(release.own_sources_text(pending), text)

    def test_versions(self):
        self.assertEqual(release.release_version("mas33", 0), "0.33.0")
        self.assertEqual(release.release_version("mas40", 2), "0.40.2")
        # mas32 was published as 37.2.10-mas32 (= 0.32.0): only a repackage is allowed
        self.assertEqual(release.release_version("mas32", 1), "0.32.1")
        for build, patch in [("mas32", 0), ("mas33", 1000), ("33", 0)]:
            with self.assertRaises(ValueError):
                release.release_version(build, patch)
        self.assertGreater(checks.version_key("v0.100.0"), checks.version_key("v0.99.0"))
        self.assertGreater(checks.version_key("v0.33.0"), checks.version_key("v0.32.1"))
        for bad in ["0.33.0", "v37.2.10-mas32", "v0.33", "v00.33.0"]:
            with self.assertRaises(ValueError):
                checks.version_key(bad)
        # since 0.41 the version is given on its own (--version); without it the old rule from --build and --patch
        version = lambda build, patch=0, given=None: release.public_version(SimpleNamespace(build=build, patch=patch, version=given))
        self.assertEqual(version("mas40", given="0.41.0"), "0.41.0")
        self.assertEqual(version("mas41", given="0.42.0"), "0.42.0")
        self.assertEqual(version("mas40", given="0.41.1"), "0.41.1")
        # --version is not checked against the build at all, also below FIRST_NEW_BUILD
        self.assertEqual(version("mas32", given="0.41.0"), "0.41.0")
        self.assertEqual(version("mas40"), "0.40.0")
        self.assertEqual(version("mas40", 2), "0.40.2")
        self.assertEqual(release.public_version(SimpleNamespace(build="mas39", patch=0)), "0.39.0")
        for given, patch in [("0.41.0", 1), ("v0.41.0", 0), ("0.41", 0), ("0.041.0", 0), ("0.41.0-rc1", 0)]:
            with self.assertRaises(ValueError):
                version("mas40", patch, given)
        for good in [1, 40, 1000]:
            self.assertTrue(checks.is_build(good))
        for bad in [0, -1, "40", 40.0, True, None]:
            self.assertFalse(checks.is_build(bad))

    def annotated_series(self, series, recorded):
        template = '<!-- mas-30 emulator 37.2.10 2026-10-01 生成 -->\n<manifest>\n'
        for s in series:
            template += (f"  <!-- {s['repo']}：AOSP 基准 {'0' * 40}，本地分支 old = 基准 + 1 个提交，HEAD {'1' * 40} -->\n"
                         f'  <project path="{s["repo"]}" revision="{"1" * 40}"/>\n')
        template += '</manifest>\n'
        expected = ET.fromstring(template)
        for p, s in zip(expected.findall("project"), series):
            p.set("revision", s["head"])
        original = json.dumps([series, recorded], sort_keys=True)
        text = release.annotated_manifest(template, expected, "mas36", series, "37.2.10", recorded)
        self.assertEqual(json.dumps([series, recorded], sort_keys=True), original)
        self.assertEqual(release.manifest_shape(ET.fromstring(text)), release.manifest_shape(expected))
        return text

    def test_annotated_manifest_reuses_recorded_directory(self):
        s = {"repo": "external/example", "base": "a" * 40, "head": "b" * 40, "dir": "example/mas36", "count": 2}
        recorded = [{**s, "head": "c" * 40, "dir": "example/mas37"}, {**s, "dir": "example/mas35"}]
        text = self.annotated_series([s], recorded)
        self.assertIn("补丁系列 patches/example/mas35 = 基准 + 2 个提交", text)
        self.assertNotIn("patches/example/mas36", text)
        self.assertNotIn("patches/example/mas37", text)

    def test_annotated_manifest_uses_new_directory(self):
        s = {"repo": "external/example", "base": "a" * 40, "head": "b" * 40, "dir": "example/mas36", "count": 3}
        # Neither a different base nor a different repository identifies this series, even with the same HEAD.
        recorded = [{**s, "base": "c" * 40, "dir": "example/mas35"},
                    {**s, "repo": "external/other", "dir": "other/mas35"}]
        text = self.annotated_series([s], recorded)
        self.assertIn("补丁系列 patches/example/mas36 = 基准 + 3 个提交", text)

    def test_annotated_manifest_mixed_directories_match_series_json(self):
        series = [{"repo": f"external/{name}", "base": "a" * 40, "head": head * 40,
                   "dir": name + "/mas36", "count": 1} for name, head in [("one", "b"), ("two", "c"), ("three", "d")]]
        recorded = [{**s, "dir": directory} for s, directory in
                    zip(series, ["one/mas36", "two/mas35", "three/module-version22"])]
        text = self.annotated_series(series, recorded)
        comments = dict(re.findall(r"<!-- (\S+)：AOSP 基准 [0-9a-f]{40}，补丁系列 patches/(\S+) =", text))
        self.assertEqual(comments, {s["repo"]: s["dir"] for s in recorded})

    def test_sources_uses_release_checkout_patch_directories(self):
        root = self.folder()
        tools, repo, baseline, out = [root / name for name in ["tools", "pub", "baseline", "out"]]
        s = {"repo": "external/example", "base": "a" * 40, "head": "b" * 40, "dir": "example/mas36", "count": 1}
        release.save(tools / "patches/series.json", [{**s, "dir": "example/mas34"}])
        release.save(repo / "patches/series.json", [{**s, "dir": "example/mas35"}])
        # this build's own series come from the export log; patches/series.json is the checkout's, every version
        release.save(out / "logs/patch-series.json", [{**s, "tree": "c" * 40, "trees": ["c" * 40], "authors": [], "path": "patches/" + s["dir"]}])
        release.save(out / "patches/series.json", [{**s, "dir": "example/mas33"}, {**s, "dir": "example/mas35"}])
        template = (f'<!-- mas-30 emulator 37.2.10 2026-10-01 生成 -->\n<manifest>\n'
                    f'  <!-- {s["repo"]}：AOSP 基准 {s["base"]}，本地分支 old = 基准 + 1 个提交，HEAD {s["head"]} -->\n'
                    f'  <project path="{s["repo"]}" revision="{s["base"]}"/>\n</manifest>\n')
        manifest = root / "manifest-mas30.xml"
        manifest.write_text(template, encoding="utf-8")
        expected = ET.fromstring(template)
        expected.find("project").set("revision", s["head"])
        release.save(out / "snapshot.json", {"build": "mas36", "version": "0.36.0", "upstream": "37.2.10", "series": [s],
                                             "manifest_xml": ET.tostring(expected, encoding="unicode"),
                                             "own": {"modavd": {"repo": str(repo)}}})
        (tools / "config").mkdir()
        (tools / "config/source-projects.txt").write_text("", encoding="utf-8")
        (baseline / "source").mkdir(parents=True)
        (baseline / "source/manifest-aosp-base.xml").write_text(template, encoding="utf-8")
        (baseline / "source/SOURCES.txt").write_text("sources\n", encoding="utf-8")
        payload = baseline / "source/unchanged.tar.xz"
        payload.write_bytes(b"unchanged source archive")
        sums = release.sha(payload) + "  unchanged.tar.xz\n"
        (tools / "config/mas30.SHA256SUMS.txt").write_text(sums, encoding="utf-8")
        (baseline / "SHA256SUMS-sources.txt").write_text(sums, encoding="utf-8")
        with (mock.patch.object(release, "REPO", tools), mock.patch.object(release, "HERE", tools),
              mock.patch.object(release, "MANIFEST_MAS30", manifest),
              mock.patch.object(release, "own_sources", return_value={"action": "reused"}),
              mock.patch.object(release, "own_sources_text", return_value="")):
            release.sources(SimpleNamespace(output=str(out), baseline=str(baseline)))
        for name, revision in [("manifest-mas36.xml", s["head"]), ("manifest-aosp-base.xml", s["base"])]:
            text = (out / "source" / name).read_text(encoding="utf-8")
            self.assertIn("补丁系列 patches/example/mas35 =", text)
            self.assertNotIn("patches/example/mas34", text)
            self.assertNotIn("patches/example/mas36", text)
            self.assertEqual(ET.fromstring(text).find("project").get("revision"), revision)
        self.assertEqual(release.sha(out / "source/unchanged.tar.xz"), release.sha(payload))

    def test_annotated_manifest_mas36(self):
        root = self.folder()
        published = release.REPO / "manifests/manifest-mas36.xml"
        expected = ET.fromstring(published.read_bytes())
        revisions = {p.get("path"): p.get("revision") for p in expected.findall("project")}
        recorded = release.load(release.REPO / "patches/series.json")
        # Reconstruct the committed build input. snapshot_data gives every series the current build's directory.
        series = []
        for s in release.load(HERE / "config/mas30.json")["series"]:
            actual = next(r for r in recorded if (r["repo"], r["base"], r["head"]) ==
                          (s["repo"], s["base"], revisions[s["repo"]]))
            series.append({**actual, "dir": s["dir"].split("/")[0] + "/mas36"})
        tracked = [published, release.REPO / "patches/series.json"]
        tracked += sorted((release.REPO / "release").glob("0.36.0.*"))
        patches = []
        for s in series:
            actual = next(r for r in recorded if (r["repo"], r["base"], r["head"]) == (s["repo"], s["base"], s["head"]))
            files = sorted((release.REPO / "patches" / actual["dir"]).glob("*.patch"))
            self.assertEqual(len(files), s["count"])
            patches += files
        tracked += patches
        before = {str(p): release.sha(p) for p in tracked}
        template = release.MANIFEST_MAS30.read_text(encoding="utf-8")
        base = ET.fromstring(published.read_bytes())
        for p in base.findall("project"):
            for s in series:
                if p.get("path") == s["repo"]:
                    p.set("revision", s["base"])
        manifests = [("manifest-mas36.xml", expected), ("manifest-aosp-base.xml", base)]
        directories = {s["repo"]: s["dir"] for s in recorded if any(
            (s["repo"], s["base"], s["head"]) == (r["repo"], r["base"], r["head"]) for r in series)}
        self.assertEqual(directories, {"external/qemu": "qemu/mas36", "hardware/google/gfxstream": "gfxstream/mas35",
                                       "external/crashpad": "crashpad/module-version22"})
        for name, tree in manifests:
            text = release.annotated_manifest(template, tree, "mas36", series, "37.2.10", recorded)
            (root / name).write_text(text, encoding="utf-8")
            comments = dict(re.findall(r"<!-- (\S+)：AOSP 基准 [0-9a-f]{40}，补丁系列 patches/(\S+) =", text))
            self.assertEqual(comments, directories)
            for directory in comments.values():
                self.assertTrue((release.REPO / "patches" / directory).is_dir(), directory)
            self.assertEqual(release.manifest_shape(ET.fromstring(text)), release.manifest_shape(tree))
            if name == published.name:
                normalize = lambda value: re.sub(r"\d{4}-\d{2}-\d{2}(?= 生成)|(?<=补丁系列 patches/)\S+", "", value)
                self.assertEqual(normalize(text), normalize(published.read_text(encoding="utf-8")))
        self.assertEqual({str(p): release.sha(p) for p in tracked}, before)
        release.save(root / "comparison.json", {"directories": directories, "patches_unchanged": len(patches),
                                               "published_files_unchanged": len(tracked) - len(patches),
                                               "project_revisions_unchanged": len(revisions),
                                               "published_manifest_sha256": release.sha(published),
                                               "regenerated_sha256": {name: release.sha(root / name) for name, _ in manifests}})

    def test_patch_export_keeps_only_noreply_contributors(self):
        # A non-ASCII name makes format-patch fold the encoded From: header over several lines.
        contributor = "贡献者" * 5 + " Example <12345+example-user@users.noreply.github.com>"
        for author in [checks.MAINTAINER, contributor, "B <b@users.noreply.github.com>"]:
            self.assertTrue(checks.author_allowed(author), author)
        for author in ["B <b@example.com>", "worker <w@localhost>", "B <1+b@users.noreply.github.com.example>",
                       "<1+b@users.noreply.github.com>", "B\n <1+b@users.noreply.github.com>"]:
            self.assertFalse(checks.author_allowed(author), author)
        root = self.folder()
        repo = root / "src"
        repo.mkdir()
        git = lambda *a: release.git(repo, *a)
        git("init", "-q")
        git("config", "user.name", "本地" * 20)
        git("config", "user.email", "worker@localhost")
        for n, (message, author) in enumerate([("base", None),
                                               ("local change\n\nDetails.\nFrom: a body line\n\nCo-authored-by: S <s@example.com>", None),
                                               ("outside fix", contributor)]):
            (repo / "f.txt").write_text(f"{n}\n", encoding="utf-8")
            git("add", "f.txt")
            git("commit", "-q", "-m", message, *(["--author", author] if author else []))
            if n == 0:
                base = git("rev-parse", "HEAD")
        out = root / "out"
        release.save(out / "snapshot.json", {"build": "mas37", "series": [
            {"repo": "src", "dir": "test/one", "base": base, "head": git("rev-parse", "HEAD"), "upstream": "platform/test"}]})
        args = SimpleNamespace(output=str(out), emu_src=str(root))
        release.export_patches(args)
        release.replay(args)
        self.assertEqual(release.load(out / "logs/patch-series.json")[0]["authors"], [checks.MAINTAINER, contributor])
        self.assertEqual(release.load(out / "logs/patch-replay.json")[0]["authors"], sorted([checks.MAINTAINER, contributor]))
        self.assertNotIn("authors", release.load(out / "patches/series.json")[0])
        local, outside = [p.read_bytes() for p in sorted((out / "patches/test/one").glob("*.patch"))]
        self.assertIn(b"\nFrom: " + checks.MAINTAINER.encode() + b"\n", local)
        self.assertNotIn(b"localhost", local)
        self.assertNotIn(b"=?UTF-8?", local.split(b"\n\n", 1)[0])
        self.assertNotIn(b"Co-authored-by", local)
        self.assertIn(b"\nFrom: a body line\n", local)
        self.assertIn(b"<12345+example-user@users.noreply.github.com>", outside)
        # git am would publish a name over 60 bytes as the bare address: refuse before exporting.
        git("commit", "-q", "--amend", "--no-edit", "--author", "贡献者" * 6 + " Example <12345+example-user@users.noreply.github.com>")
        release.save(root / "out2/snapshot.json", {"build": "mas37", "series": [
            {"repo": "src", "dir": "test/one", "base": base, "head": git("rev-parse", "HEAD"), "upstream": "platform/test"}]})
        with self.assertRaises(ValueError):
            release.export_patches(SimpleNamespace(output=str(root / "out2"), emu_src=str(root)))

    def test_patch_export_keeps_upstream_cherry_pick_authors(self):
        upstream = "Upstream Dev <dev@example.org>"
        pick = "(cherry picked from commit " + "a" * 40 + ")"
        self.assertTrue(checks.author_allowed(upstream, f"upstream fix\n\nChange-Id: I1\n{pick}\n"))
        for author, message in [(upstream, None), (upstream, "manual port\n"),
                                (upstream, "x\n\n(cherry picked from commit abc)\n"), (upstream, f"x\n\nsee {pick}\n"),
                                ("Upstream Dev <dev@localhost>", f"x\n\n{pick}\n"), ("U <dev@example.org>", f"x\n\n{pick}\n"),
                                ("<dev@example.org>", f"x\n\n{pick}\n"), ("A <b@c.d> <e@f.g>", f"x\n\n{pick}\n")]:
            self.assertFalse(checks.author_allowed(author, message), (author, message))
        entry = lambda author, message: f"{author}\x00{message}\x01\n"
        self.assertTrue(checks.commits_allowed(entry(upstream, f"fix\n\n{pick}\n") + entry(checks.MAINTAINER, "local\n")))
        self.assertFalse(checks.commits_allowed(entry(upstream, "fix\n") + entry(checks.MAINTAINER, f"local\n\n{pick}\n")))
        self.assertFalse(checks.commits_allowed(entry(checks.MAINTAINER, "local\n\nCo-authored-by: S <s@example.com>\n")))
        root = self.folder()
        repo = root / "src"
        repo.mkdir()
        git = lambda *a: release.git(repo, *a)
        git("init", "-q")
        git("config", "user.name", "Local Worker")
        git("config", "user.email", "worker@localhost")
        commits = [("base", None),
                   (f"upstream fix\n\nChange-Id: I1\n{pick}", upstream),           # someone else's, picked: kept
                   (f"own change taken from another branch\n\n{pick}", None),     # the maintainer's own pick: IDENTITY
                   ("upstream fix ported by hand", upstream)]                      # no pick line: IDENTITY
        for n, (message, author) in enumerate(commits):
            (repo / "f.txt").write_text(f"{n}\n", encoding="utf-8")
            git("add", "f.txt")
            git("commit", "-q", "-m", message, *(["--author", author] if author else []))
            if n == 0:
                base = git("rev-parse", "HEAD")
        out = root / "out"
        release.save(out / "snapshot.json", {"build": "mas38", "series": [
            {"repo": "src", "dir": "test/one", "base": base, "head": git("rev-parse", "HEAD"), "upstream": "platform/test"}]})
        args = SimpleNamespace(output=str(out), emu_src=str(root))
        release.export_patches(args)
        release.replay(args)
        self.assertEqual(release.load(out / "logs/patch-series.json")[0]["authors"],
                         [upstream, checks.MAINTAINER, checks.MAINTAINER])
        self.assertEqual(release.load(out / "logs/patch-replay.json")[0]["authors"], sorted([checks.MAINTAINER, upstream]))
        picked, own, ported = [p.read_bytes() for p in sorted((out / "patches/test/one").glob("*.patch"))]
        self.assertIn(b"\nFrom: " + upstream.encode() + b"\n", picked)
        for data in (own, ported):
            self.assertIn(b"\nFrom: " + checks.MAINTAINER.encode() + b"\n", data)
            self.assertNotIn(b"localhost", data.split(b"\n\n", 1)[0])
        self.assertNotIn(b"dev@example.org", ported)
        # The replay checks each author against its own message: the hand port under the upstream author fails,
        # even when the recorded author list agrees.
        log = release.load(out / "logs/patch-series.json")
        log[0]["authors"][2] = upstream
        release.save(out / "logs/patch-series.json", log)
        ported_path = sorted((out / "patches/test/one").glob("*.patch"))[2]
        ported_path.write_bytes(ported.replace(b"\nFrom: " + checks.MAINTAINER.encode() + b"\n", b"\nFrom: " + upstream.encode() + b"\n"))
        # Move the first replay aside: git leaves its objects read-only, which rmtree cannot delete on Windows.
        (out / "work/replay").rename(out / "work/replay-first")
        with self.assertRaises(ValueError):
            release.replay(args)
        # An upstream author git am would mangle, or one with a local address, stops the export.
        for bad in ["dev@example.org <dev@example.org>", "Upstream Dev <dev@localhost>"]:
            git("commit", "-q", "--amend", "--no-edit", "--author", bad)
            git("commit", "-q", "--amend", "-m", f"upstream fix ported by hand\n\n{pick}")
            dest = root / ("out-" + hashlib.sha256(bad.encode()).hexdigest()[:8])
            release.save(dest / "snapshot.json", {"build": "mas38", "series": [
                {"repo": "src", "dir": "test/one", "base": base, "head": git("rev-parse", "HEAD"), "upstream": "platform/test"}]})
            with self.assertRaises(ValueError):
                release.export_patches(SimpleNamespace(output=str(dest), emu_src=str(root)))

    def published_checkout(self, root):
        """Two source repositories and a release checkout that published them as 0.38.0: one/mas38 (2 patches) and
        two/v22 (1 patch). Returns (snapshot series for a mas39 build, checkout); one gets a third commit, two stays."""
        commits = {"one": ["base", "fix a", "fix b"], "two": ["base", "fix c"]}
        bases = {}
        for name, messages in commits.items():
            for n, message in enumerate(messages):
                self.commit_patch(root / name, f"{name}.txt", f"{n}\n", message, checks.MAINTAINER)
                if n == 0:
                    bases[name] = release.git(root / name, "rev-parse", "HEAD")
        series = lambda dirs: [{"repo": name, "dir": dirs[name], "base": bases[name], "head": release.git(root / name, "rev-parse", "HEAD"),
                                "upstream": f"platform/{name}"} for name in commits]
        out38 = root / "out38"
        release.save(out38 / "snapshot.json", {"build": "mas38", "series": series({"one": "one/mas38", "two": "two/mas38"})})
        release.export_patches(SimpleNamespace(output=str(out38), emu_src=str(root)))
        published = release.load(out38 / "patches/series.json")
        published[1]["dir"] = "two/v22"
        rows = "\n".join(f"| `{s['dir']}` | `{s['upstream']}` | `{s['base']}` | {s['count']} | `{s['head']}` | Apache-2.0 |" for s in published)
        base_md = ("# ModAVD 补丁\n\n各版本用的目录：\n\n| 版本 | one | two |\n| --- | --- | --- |\n"
                   "| 0.38.0（`Pkg.BuildId=mas-38`） | one/mas38 | two/v22 |\n\n"
                   "| 目录 | AOSP 仓库 | 基准（AOSP 公开提交） | 补丁数 | 回放后的树 = 编译时分支头 | 许可证 |\n"
                   "| --- | --- | --- | --- | --- | --- |\n" + rows + "\n\n"
                   "- `one/mas38` 的 0001–0002 是 mas-38 的。\n- 其余说明。\n\n## 许可证\n\n正文。\n")
        files = {"README.md": b"# ModAVD\n", "patches/BASE.md": base_md.encode(),
                 "patches/series.json": (json.dumps(published, ensure_ascii=False, indent=2) + "\n").encode()}
        for old, new in [("one/mas38", "one/mas38"), ("two/mas38", "two/v22")]:
            for p in sorted((out38 / "patches" / old).glob("*.patch")):
                files[f"patches/{new}/{p.name}"] = p.read_bytes()
        self.commit_files(root / "pub", files)
        self.commit_patch(root / "one", "one.txt", "3\n", "fix d", checks.MAINTAINER)
        return series({"one": "one/mas39", "two": "two/mas39"}), root / "pub"

    def export_into(self, root, name, series, checkout, version="0.39.0"):
        out = root / name
        release.save(out / "snapshot.json", {"build": "mas39", "version": version, "series": series,
                                             "own": {"modavd": {"repo": str(checkout)}}})
        release.export_patches(SimpleNamespace(output=str(out), emu_src=str(root)))
        return out

    def test_patch_records_add_to_the_published_ones(self):
        """0.38.0: the release commit took the export's own short BASE.md and series.json, which dropped every older
        row and the license section, and exported the unchanged Crashpad series again as crashpad/mas38."""
        root = self.folder()
        series, checkout = self.published_checkout(root)
        published = release.load(checkout / "patches/series.json")
        before = (checkout / "patches/BASE.md").read_text(encoding="utf-8")
        out = self.export_into(root, "out", series, checkout)
        records = release.load(out / "patches/series.json")
        self.assertEqual([s["dir"] for s in records], ["one/mas39", "one/mas38", "two/v22"])
        self.assertEqual(records[1:], published)
        self.assertEqual((records[0]["count"], records[0]["head"]), (3, series[0]["head"]))
        self.assertNotIn("path", records[0])
        self.assertEqual(release.load(out / "logs/patch-dirs.json"),
                         {"one": {"dir": "one/mas39", "new": True}, "two": {"dir": "two/v22", "new": False}})
        # only the new directory is left to copy; the unchanged series was exported for the replay, elsewhere
        self.assertEqual(sorted(p.relative_to(out / "patches").as_posix() for p in (out / "patches").rglob("*") if p.is_dir()),
                         ["one", "one/mas39"])
        self.assertEqual(len(list((out / "work/patches-reused/two/mas39").glob("*.patch"))), 1)
        log = release.load(out / "logs/patch-series.json")
        self.assertEqual([s["path"] for s in log], ["patches/one/mas39", "work/patches-reused/two/mas39"])
        text = (out / "patches/BASE.md").read_text(encoding="utf-8")
        lines = text.split("\n")
        # nothing published is lost or moved: the old text is the new one with lines added
        remaining = iter(lines)
        self.assertTrue(all(line in remaining for line in before.split("\n")))
        self.assertEqual(lines[lines.index("| --- | --- | --- |") + 1], "| 0.39.0（`Pkg.BuildId=mas-39`） | one/mas39 | two/v22 |")
        row = next(i for i, line in enumerate(lines) if line.startswith("| `one/mas39` |"))
        self.assertTrue(lines[row + 1].startswith("| `one/mas38` |"))
        self.assertIn(release.PLACEHOLDER, lines[row])
        self.assertIn("- `one/mas39` 的 0001–0002 与 `one/mas38` 是同一批提交（补丁文件头里的序号不同），0003–0003 是 mas-39 新增的："
                      + release.PLACEHOLDER, text)
        self.assertIn("- 0.39.0 的 two 补丁和已发布的 `two/v22` 是同一批提交（基准、分支头都相同），沿用这个目录", text)
        self.assertLess(text.index("- `one/mas39` 的"), text.index("- `one/mas38` 的"))
        for s in records:
            checks.check_base_row(text, s)
        # the public check refuses it until the new rows are filled in
        (root / "filled/patches").mkdir(parents=True)
        (root / "filled/patches/BASE.md").write_text(text, encoding="utf-8")
        with self.assertRaisesRegex(ValueError, release.PLACEHOLDER):
            checks.check_base(root / "filled")
        (root / "filled/patches/BASE.md").write_text(text.replace(release.PLACEHOLDER, "Apache-2.0"), encoding="utf-8")
        checks.check_base(root / "filled")
        # the release commit took them; running again on that adds nothing
        files = {"patches/BASE.md": text.replace(release.PLACEHOLDER, "已填").encode(),
                 "patches/series.json": (out / "patches/series.json").read_bytes()}
        for p in sorted((out / "patches/one/mas39").glob("*.patch")):
            files[f"patches/one/mas39/{p.name}"] = p.read_bytes()
        self.commit_files(checkout, files)
        again = self.export_into(root, "again", series, checkout)
        self.assertEqual(release.load(again / "patches/series.json"), records)
        self.assertEqual((again / "patches/BASE.md").read_text(encoding="utf-8"), text.replace(release.PLACEHOLDER, "已填"))
        self.assertEqual(release.load(again / "logs/patch-dirs.json"),
                         {"one": {"dir": "one/mas39", "new": False}, "two": {"dir": "two/v22", "new": False}})
        # a repack of the same build only names its version in that row
        repack = self.export_into(root, "repack", series, checkout, "0.39.1")
        repacked = (repack / "patches/BASE.md").read_text(encoding="utf-8")
        self.assertIn("| 0.39.0（`Pkg.BuildId=mas-39`；0.39.1 相同） | one/mas39 | two/v22 |", repacked)
        self.assertEqual(repacked.replace("；0.39.1 相同", ""), (again / "patches/BASE.md").read_text(encoding="utf-8"))
        self.assertEqual(release.load(repack / "patches/series.json"), records)
        # since 0.41 a version of its own can ship a published build (0.41.0 on mas-40): the row is found by the
        # build, not by the middle of the version, and only names the version
        later = self.export_into(root, "later", series, checkout, "0.41.0")
        text_later = (later / "patches/BASE.md").read_text(encoding="utf-8")
        self.assertIn("| 0.39.0（`Pkg.BuildId=mas-39`；0.41.0 相同） | one/mas39 | two/v22 |", text_later)
        self.assertEqual(text_later.replace("；0.41.0 相同", ""), (again / "patches/BASE.md").read_text(encoding="utf-8"))
        self.assertEqual(release.load(later / "logs/patch-dirs.json"),
                         {"one": {"dir": "one/mas39", "new": False}, "two": {"dir": "two/v22", "new": False}})
        # the version is matched whole: 0.39.1 is not named by 0.39.10, and 0.39.0 by itself adds nothing
        self.commit_files(checkout, {"patches/BASE.md": text_later.replace("；0.41.0 相同", "；0.39.10 相同").encode()})
        self.assertIn("；0.39.10 相同；0.39.1 相同）", (self.export_into(root, "whole", series, checkout, "0.39.1")
                                                      / "patches/BASE.md").read_text(encoding="utf-8"))
        self.assertNotIn("0.39.0 相同", (self.export_into(root, "same", series, checkout, "0.39.0")
                                       / "patches/BASE.md").read_text(encoding="utf-8"))

    def test_patch_records_update_the_qemu_license_section(self):
        """A new qemu series that continues the previous one: the license section's list of commits and the replay
        example follow (0.38.0 did both by hand)."""
        old = {"repo": "external/qemu", "upstream": "platform/external/qemu", "base": "a" * 40, "head": "b" * 40,
               "dir": "qemu/mas38", "count": 2, "tree": "c" * 40}
        s = {**old, "head": "d" * 40, "dir": "qemu/mas39", "count": 3, "tree": "e" * 40, "trees": [], "authors": [], "path": "patches/qemu/mas39"}
        base_md = ("| 版本 | qemu |\n| --- | --- |\n| 0.38.0（`Pkg.BuildId=mas-38`） | qemu/mas38 |\n\n"
                   "| 目录 | AOSP 仓库 | 基准 | 补丁数 | 分支头 | 许可证 |\n| --- | --- | --- | --- | --- | --- |\n"
                   f"| `qemu/mas38` | `platform/external/qemu` | `{'a' * 40}` | 2 | `{'b' * 40}` | GPL-2.0 |\n\n- 说明。\n\n"
                   "## qemu 补丁的许可证\n\n这张表是按编译时提交上各文件开头的许可证声明逐个归出来的（0001–0002 按 `bbbbbbbb`）。\n\n"
                   "回放示例（以 mas-38 的 qemu 为例）：\n\n```sh\ngit checkout -b mas38 " + "a" * 40 +
                   "\ngit am --keep-cr ../patches/qemu/mas38/*.patch\n```\n")
        root = self.folder()
        with mock.patch.object(release, "leading_same", return_value=2):
            records, text, dirs = release.merge_patch_records([s], [old], base_md, root, root, "0.39.0", 39)
        self.assertIn("（0001–0002 按 `bbbbbbbb`，0003–0003 按 `dddddddd`）。", text)
        self.assertIn("回放示例（以 mas-39 的 qemu 为例）：", text)
        self.assertIn("git checkout -b mas39 " + "a" * 40 + "\ngit am --keep-cr ../patches/qemu/mas39/*.patch\n", text)
        self.assertEqual([r["dir"] for r in records], ["qemu/mas39", "qemu/mas38"])
        # a series that starts over (nothing in common with the previous one) leaves both for a person
        with mock.patch.object(release, "leading_same", return_value=0):
            _, text, _ = release.merge_patch_records([s], [old], base_md, root, root, "0.39.0", 39)
        self.assertIn("（0001–0002 按 `bbbbbbbb`）。", text)
        self.assertIn("回放示例（以 mas-38 的 qemu 为例）：", text)

    def test_patch_records_refuse_a_stale_directory(self):
        root = self.folder()
        series, checkout = self.published_checkout(root)
        stale = release.load(checkout / "patches/series.json")
        stale.insert(0, {**stale[0], "dir": "one/mas39", "head": "f" * 40, "count": 7})
        self.commit_files(checkout, {"patches/series.json": (json.dumps(stale, ensure_ascii=False, indent=2) + "\n").encode()})
        with self.assertRaisesRegex(ValueError, "already in series.json"):
            self.export_into(root, "out", series, checkout)
        # a published series whose base and head match but whose count or tree does not is refused too
        broken = release.load(checkout / "patches/series.json")[1:]
        broken[1]["tree"] = "0" * 40
        self.commit_files(checkout, {"patches/series.json": (json.dumps(broken, ensure_ascii=False, indent=2) + "\n").encode()})
        with self.assertRaisesRegex(ValueError, "another count or tree"):
            self.export_into(root, "out2", series, checkout)

    CONTRIBUTOR = "A Contributor <12345+example-user@users.noreply.github.com>"

    def commit_patch(self, repo, name, content, message, author):
        """Commit `content` to `name` in a fresh-or-existing repo and return that commit's format-patch bytes."""
        git = lambda *a: release.git(repo, *a)
        if not (repo / ".git").exists():
            repo.mkdir(parents=True, exist_ok=True)
            git("init", "-q")
            git("config", "user.name", "Local Worker")
            git("config", "user.email", "worker@localhost")
        (repo / name).write_text(content, encoding="utf-8")
        git("add", name)
        git("commit", "-q", "-m", message, "--author", author)
        return release.run("git", "-C", repo, "format-patch", "-1", "--stdout")

    def test_incoming_layout_and_authors(self):
        root = self.folder()
        good = self.commit_patch(root / "src", "f.txt", "1\n", "outside fix", self.CONTRIBUTOR)
        def check(files):
            repo = root / f"repo{len(list(root.glob('repo*')))}"
            for rel, data in files.items():
                (repo / rel).parent.mkdir(parents=True, exist_ok=True)
                (repo / rel).write_bytes(data)
            return checks.check_incoming(repo)
        self.assertEqual(check({"incoming/README.md": b"x", "incoming/fix-a/0001-fix.patch": good,
                                "incoming/fix-a/tests/probe.py": b"x"}), 1)
        self.assertEqual(check({}), 0)
        bad = [{"incoming/fix-a/notes.txt": b"x"},
               {"incoming/0001-fix.patch": good},
               {"incoming/fix-a/0001-fix.patch": b"not a patch"},
               {"incoming/fix-a/0001-fix.patch": self.commit_patch(root / "src", "f.txt", "2\n", "local", "W <w@localhost>")},
               {"incoming/fix-a/0001-fix.patch": self.commit_patch(root / "src", "f.txt", "3\n", "x\n\nCo-authored-by: S <s@example.com>", self.CONTRIBUTOR)},
               {"incoming/fix-a/0001-fix.patch": self.commit_patch(root / "src", "f.txt", "4\n", "x", "名" * 21 + " <1+b@users.noreply.github.com>")}]
        for files in bad:
            with self.assertRaises(ValueError, msg=str(list(files))):
                check(files)

    def test_incoming_found_in_series_and_removed(self):
        root = self.folder()
        src = root / "src"
        first = self.commit_patch(src, "f.txt", "0\n", "base", "Local Worker <worker@localhost>")
        base = release.git(src, "rev-parse", "HEAD")
        outside = self.commit_patch(src, "f.txt", "1\n", "outside fix", self.CONTRIBUTOR)
        self.commit_patch(src, "g.txt", "a\nb\nc\n", "second fix", self.CONTRIBUTOR)
        # The merged copy of "second fix" was made against other context, as if the import needed a rebase.
        rebased = self.commit_patch(root / "other", "g.txt", "z\nb\n", "second fix", self.CONTRIBUTOR)
        never = self.commit_patch(root / "other", "h.txt", "1\n", "never imported", self.CONTRIBUTOR)
        out = root / "out"
        release.save(out / "snapshot.json", {"build": "mas37", "series": [
            {"repo": "src", "dir": "test/one", "base": base, "head": release.git(src, "rev-parse", "HEAD"), "upstream": "platform/test"}]})
        release.export_patches(SimpleNamespace(output=str(out), emu_src=str(root)))
        pub = root / "pub"
        for rel, data in {"README.md": b"# ModAVD\n", "patches/BASE.md": b"x\n", "incoming/fix-a/0001-fix.patch": outside,
                          "incoming/fix-b/0001-fix.patch": rebased, "incoming/fix-b/tests/probe.py": b"x",
                          "incoming/fix-c/0001-fix.patch": never}.items():
            (pub / rel).parent.mkdir(parents=True, exist_ok=True)
            (pub / rel).write_bytes(data)
        release.git(pub, "init", "-q")
        release.git(pub, "add", "-A")
        release.git(pub, "-c", "user.name=t", "-c", "user.email=t@example.com", "commit", "-q", "-m", "merged")
        args = SimpleNamespace(output=str(out), repository=str(pub), keep_incoming=[])
        with self.assertRaises(ValueError):
            release.incoming(args)
        self.assertTrue((pub / "incoming/fix-a").exists())
        release.incoming(SimpleNamespace(output=str(out), repository=str(pub), keep_incoming=["fix-c"]))
        log = {e["topic"]: e for e in release.load(out / "logs/incoming.json")}
        self.assertEqual((log["fix-a"]["matched_by"], log["fix-b"]["matched_by"]), ("patch-id", "author+subject"))
        self.assertTrue(log["fix-c"]["kept"])
        self.assertEqual([p.name for p in (pub / "incoming").iterdir()], ["fix-c"])
        self.assertEqual(sorted(release.git(pub, "diff", "--cached", "--name-only").splitlines()),
                         ["incoming/fix-a/0001-fix.patch", "incoming/fix-b/0001-fix.patch", "incoming/fix-b/tests/probe.py"])

    def test_incoming_removed_from_crlf_checkout(self):
        """0.38.0: the release checkout was cloned with core.autocrlf=true, so the text files of a topic other than
        its patches (*.patch -text) were CRLF in the work tree. git rm under GIT_ENV's core.autocrlf=false took them
        for local modifications and refused; the removal must use the checkout's own line-ending settings."""
        root = self.folder()
        src = root / "src"
        self.commit_patch(src, "f.txt", "0\n", "base", "Local Worker <worker@localhost>")
        base = release.git(src, "rev-parse", "HEAD")
        outside = self.commit_patch(src, "f.txt", "1\n", "outside fix", self.CONTRIBUTOR)
        out = root / "out"
        release.save(out / "snapshot.json", {"build": "mas39", "series": [
            {"repo": "src", "dir": "test/one", "base": base, "head": release.git(src, "rev-parse", "HEAD"), "upstream": "platform/test"}]})
        release.export_patches(SimpleNamespace(output=str(out), emu_src=str(root)))
        self.commit_files(root / "pub", {".gitattributes": b"*.patch -text\n", "README.md": b"# ModAVD\n", "patches/BASE.md": b"x\n",
                                         "incoming/fix-a/0001-fix.patch": outside,
                                         "incoming/fix-a/tests/probe.py": b"line one\nline two\n"})
        clone = root / "clone"
        release.run("git", "-c", "core.autocrlf=true", "clone", "-q", "-c", "core.autocrlf=true", root / "pub", clone,
                    env=release.STATUS_ENV)
        probe = clone / "incoming/fix-a/tests/probe.py"
        self.assertIn(b"\r\n", probe.read_bytes())
        # make git compare the content instead of trusting the index, as it does for a fresh clone
        later = probe.stat().st_mtime + 60
        os.utime(probe, (later, later))
        release.incoming(SimpleNamespace(output=str(out), repository=str(clone), keep_incoming=[]))
        self.assertFalse((clone / "incoming/fix-a").exists())
        self.assertEqual(sorted(release.git(clone, "diff", "--cached", "--name-only").splitlines()),
                         ["incoming/fix-a/0001-fix.patch", "incoming/fix-a/tests/probe.py"])
        # a real local change still stops the removal
        self.commit_files(root / "pub2", {".gitattributes": b"*.patch -text\n", "README.md": b"# ModAVD\n", "patches/BASE.md": b"x\n",
                                          "incoming/fix-a/0001-fix.patch": outside,
                                          "incoming/fix-a/tests/probe.py": b"line one\n"})
        changed_clone = root / "clone2"
        release.run("git", "-c", "core.autocrlf=true", "clone", "-q", "-c", "core.autocrlf=true", root / "pub2", changed_clone,
                    env=release.STATUS_ENV)
        (changed_clone / "incoming/fix-a/tests/probe.py").write_bytes(b"edited by hand\r\n")
        with self.assertRaisesRegex(RuntimeError, "local modifications"):
            release.incoming(SimpleNamespace(output=str(out), repository=str(changed_clone), keep_incoming=[]))
        self.assertTrue((changed_clone / "incoming/fix-a/tests/probe.py").exists())

    def notes_text(self, changes=None, upper="## 更新内容\n\n- x", version="v0.33.0", build=None):
        build = checks.version_key(version)[1] if build is None else build
        data = {"version": version, "build": build, "upstream": "37.2.10", "min_automas": "5.7.0",
                "changes": changes or {version: {"修复": ["修好一处"]}, "v0.32.0": {"变更": ["读写更快"]}}}
        return checks.machine_line(data) + "\n" + upper + "\n\n" + checks.SEPARATOR + "\n\n## 下载\n"

    def test_announcement_checks(self):
        checks.check_announcement(self.notes_text(), "v0.33.0", 33)
        bad = [
            self.notes_text().replace(checks.SEPARATOR, ""),
            self.notes_text() + checks.SEPARATOR + "\n",
            self.notes_text(upper="x" * (checks.MAX_ANNOUNCEMENT_CHARS + 1)),
            self.notes_text(upper="| `a.zip` | 1 字节 | `" + "a" * 64 + "` |"),
            self.notes_text(changes={"v0.32.0": {"变更": ["a"]}, "v0.33.0": {"修复": ["b"]}}),
            self.notes_text(changes={"v0.33.0": {"其他": ["b"]}}),
            "\n" + self.notes_text(),
        ]
        for text in bad:
            with self.assertRaises(ValueError):
                checks.check_announcement(text)
        with self.assertRaises(ValueError):
            checks.check_announcement(self.notes_text(), "v0.34.0", 34)
        # since 0.41 the build is not the middle of the version
        for version, build in [("v0.41.0", 40), ("v0.42.0", 41), ("v0.41.1", 40)]:
            text = self.notes_text(changes={version: {"修复": ["a"]}, "v0.40.0": {"修复": ["b"]}}, version=version, build=build)
            self.assertEqual(checks.check_announcement(text, version, build)["build"], build)
            with self.assertRaises(ValueError):
                checks.check_announcement(text, version, build + 1)
        for build in [0, -1, "40", True, 40.0]:
            with self.assertRaises(ValueError):
                checks.check_announcement(self.notes_text(changes={"v0.41.0": {"修复": ["a"]}}, version="v0.41.0", build=build))
        with self.assertRaises(ValueError):
            checks.machine_line({"changes": {"v0.33.0": {"修复": ["a -- b"]}}})

    def test_notes_budget_before_packaging(self):
        """0.38.0: the machine line (9157) and the announcement (4532) were over their limits, which only step 7 said,
        after packaging. preflight and notes-budget measure them from changes.md and changelog.json alone."""
        path = self.folder()
        changes, changelog = path / "changes.md", path / "changelog.json"
        changes.write_text("".join(f"## {t}\n\n- 内容\n\n" for t in release.UPPER_SECTIONS + release.LOWER_SECTIONS), encoding="utf-8")
        entry = lambda b, item: {"version": f"v0.{b}.0", "build": b, "min_automas": None, "changes": {"修复": [item]}}
        release.save(changelog, [entry(39, "修好一处")] + [entry(b, "旧版") for b in range(38, 29, -1)])
        machine, upper = release.notes_budget("0.39.0", 39, "37.2.10", changes, changelog)
        fields = release.text_fields("0.39.0", 39, "37.2.10", changes, changelog)
        self.assertEqual(machine, len(fields["MACHINE"]))
        self.assertLess(upper, 400)
        # the same numbers step 7 would measure: the asset rows only sit below the separator
        text = release.render_notes({**fields, **release.SAMPLE_ASSETS})
        self.assertEqual(checks.check_announcement(text, "v0.39.0", 39)["version"], "v0.39.0")
        rendered = text.split("\n")
        self.assertEqual((machine, upper), (len(rendered[0]), sum(len(line) + 1 for line in rendered[1:rendered.index(checks.SEPARATOR)]) - 1))
        self.assertNotIn("WARNING", release.describe_budget(machine, upper))
        self.assertIn("WARNING: announcement", release.describe_budget(machine, 3700))
        # this version's items count in both; older ones only in the machine line
        release.save(changelog, [entry(39, "长" * 3990)] + [entry(b, "旧版") for b in range(38, 29, -1)])
        with self.assertRaisesRegex(ValueError, r"announcement above the separator would be \d+ chars, limit 4000"):
            release.notes_budget("0.39.0", 39, "37.2.10", changes, changelog)
        release.save(changelog, [entry(39, "修好一处")] + [entry(b, "旧" * 900) for b in range(38, 29, -1)])
        with self.assertRaisesRegex(ValueError, r"machine-readable line would be \d+ chars, limit 8000"):
            release.notes_budget("0.39.0", 39, "37.2.10", changes, changelog)
        with self.assertRaisesRegex(ValueError, r"Machine-readable line is \d+ chars"):
            checks.check_announcement(release.render_notes({**release.text_fields("0.39.0", 39, "37.2.10", changes, changelog),
                                                            **release.SAMPLE_ASSETS}))
        # the command needs no SDK or output directory
        release.save(changelog, [entry(39, "修好一处")])
        printed = io.StringIO()
        with contextlib.redirect_stdout(printed):
            release.notes_budget_command(SimpleNamespace(build="mas39", patch=0, sdk=None, changes=str(changes), changelog=str(changelog)))
        self.assertRegex(printed.getvalue(), r"v0\.39\.0: machine line \d+/8000, announcement \d+/4000")
        # since 0.41 the version is given on its own: 0.41.0 on mas-40, the line before it still 0.40.0 on mas-40
        release.save(changelog, [{**entry(40, "修好一处"), "version": "v0.41.0"}, entry(40, "旧版"), entry(39, "旧版")])
        fields = release.text_fields("0.41.0", 40, "37.2.10", changes, changelog)
        self.assertEqual((fields["VERSION"], fields["BUILD_ID"]), ("0.41.0", "mas-40"))
        text = release.render_notes({**fields, **release.SAMPLE_ASSETS})
        self.assertTrue(text.split("\n")[1].startswith("ModAVD 0.41.0，构建号 `mas-40`，"))
        self.assertEqual(checks.check_announcement(text, "v0.41.0", 40)["build"], 40)
        release.notes_budget("0.41.0", 40, "37.2.10", changes, changelog)
        printed = io.StringIO()
        with contextlib.redirect_stdout(printed):
            release.notes_budget_command(SimpleNamespace(build="mas40", patch=0, version="0.41.0", sdk=None, changes=str(changes),
                                                         changelog=str(changelog)))
        self.assertRegex(printed.getvalue(), r"v0\.41\.0: machine line")
        with self.assertRaisesRegex(ValueError, "differs from -Build"):
            release.notes_budget_command(SimpleNamespace(build="mas41", patch=0, version="0.41.0", sdk=None, changes=str(changes),
                                                         changelog=str(changelog)))

    def test_changes_and_changelog(self):
        path = self.folder()
        good = "".join(f"## {t}\n\n- 内容\n\n" for t in release.UPPER_SECTIONS + release.LOWER_SECTIONS)
        changes = path / "changes.md"
        changes.write_text(good, encoding="utf-8")
        self.assertEqual(set(release.read_changes(changes)), set(release.UPPER_SECTIONS + release.LOWER_SECTIONS))
        for bad in [good.replace("- 内容", "- （" + release.PLACEHOLDER + "）", 1), good.replace("## 已知问题", "## 其他"), "前言\n" + good]:
            changes.write_text(bad, encoding="utf-8")
            with self.assertRaises(ValueError):
                release.read_changes(changes)
        log = path / "changelog.json"
        entry = lambda v, b: {"version": v, "build": b, "min_automas": None, "changes": {"修复": ["a"]}}
        release.save(log, [entry("v0.34.0", 34), entry("v0.33.0", 33), entry("v0.32.0", 32)])
        self.assertEqual([e["version"] for e in release.read_changelog(log, "0.33.0", 33)], ["v0.33.0", "v0.32.0"])
        for bad in [[entry("v0.32.0", 32)], [entry("v0.32.0", 32), entry("v0.33.0", 33)], [entry("v0.33.0", 34)]]:
            release.save(log, bad)
            with self.assertRaises(ValueError):
                release.read_changelog(log, "0.33.0", 33)
        # since 0.41 the version counts on its own; the entries up to 0.40 still have build == the middle
        release.save(log, [entry("v0.42.0", 41), entry("v0.41.1", 40), entry("v0.41.0", 40), entry("v0.40.0", 40),
                           entry("v0.39.0", 39)])
        self.assertEqual([e["version"] for e in release.read_changelog(log, "0.41.1", 40)],
                         ["v0.41.1", "v0.41.0", "v0.40.0", "v0.39.0"])
        self.assertEqual(release.read_changelog(log, "0.42.0", 41)[0]["build"], 41)
        self.assertEqual(release.read_changelog(log, "0.41.0", 40)[0]["version"], "v0.41.0")
        for version, number in [("0.41.1", 41), ("0.42.0", 40), ("0.41.0", 41)]:
            with self.assertRaisesRegex(ValueError, "differs from -Build"):
                release.read_changelog(log, version, number)
        for build in [0, "40", True, None]:
            release.save(log, [entry("v0.41.0", 40), entry("v0.40.0", build)])
            with self.assertRaisesRegex(ValueError, "positive integer"):
                release.read_changelog(log, "0.41.0", 40)
        # still newest first, without duplicates
        for bad in [[entry("v0.41.0", 40), entry("v0.41.1", 40)], [entry("v0.41.0", 40), entry("v0.41.0", 41)]]:
            release.save(log, bad)
            with self.assertRaises(ValueError):
                release.read_changelog(log, "0.41.0", 40)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", required=True, type=Path)
    args = parser.parse_args()
    WORK = args.work
    if WORK.exists():
        parser.error("--work must be a fresh directory")
    unittest.main(argv=[sys.argv[0]], verbosity=2)
