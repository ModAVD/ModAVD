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
        self.assertEqual(list(release.component_apks(without, pins)), ["components/" + launcher.name])
        # shipping an APK nobody pinned is refused
        with self.assertRaises(ValueError):
            release.component_apks(with_ime, pins)
        pins.write_text(line(launcher, launcher.name) + line(ime, release.IME_APK), encoding="utf-8")
        self.assertEqual(sorted(release.component_apks(with_ime, pins)), sorted(["components/" + launcher.name, "components/" + release.IME_APK]))
        # once pinned it has to come every time (else the incremental update deletes it), with exactly that content
        with self.assertRaises(ValueError):
            release.component_apks(without, pins)
        ime.write_bytes(b"another build")
        with self.assertRaises(ValueError):
            release.component_apks(with_ime, pins)
        ime.write_bytes(b"ime")
        # the desktop APK cannot take the IME's name, not even in another case (NTFS ignores case)
        only_ime = path / "only-ime.SHA256SUMS.txt"
        only_ime.write_text(line(ime, release.IME_APK), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "collides"):
            release.component_apks(SimpleNamespace(apk=str(ime), ime_apk=str(ime)), only_ime)
        upper = path / "MODAVD-IME.APK"
        upper.write_bytes(b"ime")
        with self.assertRaisesRegex(ValueError, "collides"):
            release.component_apks(SimpleNamespace(apk=str(upper), ime_apk=str(ime)), only_ime)
        # pin lines are distinct components/<name>, case-insensitively
        bad = path / "bad.SHA256SUMS.txt"
        bad.write_text(line(ime, release.IME_APK) + line(upper, "MODAVD-IME.APK"), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "distinct"):
            release.component_apks(with_ime, bad)

    def test_texts_extended_and_every_package_file_named(self):
        tree = self.folder()
        sep = "=" * 59
        def entry(lib, files):
            return "\n".join([sep, "Notices for file(s):", *files, f"The notices is included for the library: {lib}", "", sep, "body", "", ""])
        icon_files, script_files = ["./emulator.exe"], ["(package root) 说明.txt", "(package root) LICENSES/NOTICE.txt",
                                                         "(package root) LICENSES/NOTICE.csv"]
        notice = ("header\n\n" + entry("ModAVD program icon (fan art)", icon_files) +
                  entry("ModAVD scripts and documents", script_files) + "#" * 79 + "\n# PART 3. Full license texts\n")
        (tree / "LICENSES").mkdir()
        (tree / "LICENSES/NOTICE.txt").write_text(notice, encoding="utf-8")
        rows = [["External Library Name", "Link to License", "License Name", "Binaries using the license", "URL", "ModAVD note"],
                ["ModAVD program icon (fan art)", "", "Not licensed (see text)", " ".join(icon_files), "", "a, quoted note"],
                ["ModAVD scripts and documents", "", "AGPL-3.0-or-later (scripts and batch files) / Apache-2.0 (documents)",
                 " ".join(script_files), "", ""]]
        (tree / "LICENSES/NOTICE.csv").write_text(release.render_csv(rows), encoding="utf-8")
        (tree / "LICENSES/Apache-2.0.txt").write_text("license text", encoding="utf-8")
        (tree / "说明.txt").write_text("安装\n\n====================\n出问题怎么办\n====================\n", encoding="utf-8")
        shipped = [*release.MODAVD_FILES.values(), "tools/modavd-bridge.ps1", "tools/report-config.ps1", "复制配置信息.bat",
                   "sdk/emulator/emulator.exe", *(f"sdk/emulator/resources/modavd/{n}" for n in
                                                  ["libmodavd_bridge.so", "libmodavd_gate.so", "modavd_bridge.rc"])]
        for rel in shipped:
            (tree / rel).parent.mkdir(parents=True, exist_ok=True)
            (tree / rel).write_bytes(b"x")
        with self.assertRaisesRegex(ValueError, "ModAVD.exe.*libmodavd_bridge.so.*tools/modavd.exe"):
            release.check_notice_coverage(tree)
        release.extend_texts(tree, "go1.26.5")
        release.check_notice_coverage(tree)
        text = (tree / "LICENSES/NOTICE.txt").read_text(encoding="utf-8")
        self.assertIn("(package root) ModAVD.exe\nThe notices is included for the library: ModAVD program icon (fan art)", text)
        self.assertIn("Go go1.26.5 (runtime and standard library)", text)
        self.assertLess(text.index("ModAVD arm64 execution bridge"), text.index("# PART 3."))
        self.assertNotIn("{{", text)
        csv_rows = list(release.csv.reader(io.StringIO((tree / "LICENSES/NOTICE.csv").read_text(encoding="utf-8"))))
        self.assertEqual(csv_rows[1][5], "a, quoted note")
        self.assertTrue(csv_rows[1][3].endswith("(package root) ModAVD.exe"))
        self.assertEqual(len(csv_rows), 6)
        self.assertIn("管理器和命令行", (tree / "说明.txt").read_text(encoding="utf-8"))
        # an entry the texts were reviewed with is gone: refuse rather than guess
        (tree / "说明.txt").write_text("安装\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "说明.txt"):
            release.extend_texts(tree, "go1.26.5")
        (tree / "说明.txt").write_text("====================\n出问题怎么办\n", encoding="utf-8")
        (tree / "LICENSES/NOTICE.txt").write_text(notice.replace("ModAVD scripts and documents", "Scripts"), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "ModAVD scripts and documents"):
            release.extend_texts(tree, "go1.26.5")
        # a file nobody listed
        (tree / "tools/new.ps1").write_bytes(b"x")
        with self.assertRaisesRegex(ValueError, "tools/new.ps1"):
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
        self.commit_files(root / "pub", {".gitattributes": b"package/** -text\nbridge/** -text\n", "README.md": b"# ModAVD\n",
                                         "patches/BASE.md": b"x\n", "bridge/nbshim/modavd_bridge.rc": b"service modavd\r\n",
                                         "bridge/nbshim/jni/native_bridge_shim.cc": b"int x;\n",
                                         **{f"package/{n}": f"{n}\r\n".encode() for n in release.PACKAGE_TOOLS + release.PACKAGE_ROOT}})
        return SimpleNamespace(manager=str(manager), manager_repo=str(root / "mgr"), ime_apk=str(apk), ime_repo=str(root / "ime"),
                               repository=str(root / "pub"))

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
        self.assertEqual((dest / "补全组件.bat").read_bytes(), "补全组件.bat\r\n".encode())
        self.assertEqual((dest / "tools/modavd-bridge.ps1").read_bytes(), b"modavd-bridge.ps1\r\n")
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
        (root / "pub/package/collect-info.ps1").write_bytes(b"changed\r\n")
        with self.assertRaisesRegex(ValueError, "uncommitted changes under package/"):
            release.public_sources(args)

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
                     "https://github.com/ModAVD/ModAVD/tree/main/bridge", "https://github.com/ModAVD/ModAVD/tree/main/package"]:
            self.assertIn(text, notice)
        self.assertEqual(names[:2], ["modavd-src/LICENSE", "modavd-src/NOTICE"])
        for name in ["modavd-src/ModAVDManager/go.mod", "modavd-src/ModAVDManager/main_test.go",
                     "modavd-src/ModAVDHostIME/ime/AndroidManifest.xml", "modavd-src/ModAVD/bridge/nbshim/jni/native_bridge_shim.cc"]:
            self.assertIn(name, names)
        self.assertFalse([n for n in names if n.startswith(("modavd-src/ModAVD/package", "modavd-src/ModAVD/README"))])
        self.assertEqual(rc, b"service modavd\r\n")
        text = release.own_sources_text(own)
        for part in ["ModAVDManager v0.2.0", "ModAVDManager-0.2.0-win-x64.zip", "Go go1.26.5", "ModAVDHostIME v1.0.0",
                     own["modavd"]["commit"][:12]]:
            self.assertIn(part, text)
        self.assertNotIn("{", text)
        self.assertNotIn("ModAVDHostIME", release.own_sources_text({**own, "ime": None}))

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
        self.assertEqual(checks.build_of("v0.32.1"), 32)
        for bad in ["0.33.0", "v37.2.10-mas32", "v0.33", "v00.33.0"]:
            with self.assertRaises(ValueError):
                checks.version_key(bad)

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
        release.save(out / "patches/series.json", [s])
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

    def notes_text(self, changes=None, upper="## 更新内容\n\n- x", version="v0.33.0"):
        data = {"version": version, "build": checks.build_of(version), "upstream": "37.2.10", "min_automas": "5.7.0",
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
        with self.assertRaises(ValueError):
            checks.machine_line({"changes": {"v0.33.0": {"修复": ["a -- b"]}}})

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


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", required=True, type=Path)
    args = parser.parse_args()
    WORK = args.work
    if WORK.exists():
        parser.error("--work must be a fresh directory")
    unittest.main(argv=[sys.argv[0]], verbosity=2)
