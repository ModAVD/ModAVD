"""Tests of release_ci.py (the release workflows' checks) against a fake GitHub; no network.

    python -I tools/pubrel/test_release_ci.py
"""
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import release_ci as ci  # noqa: E402

REPO = "ModAVD/ModAVD"
MGR = "ModAVD/ModAVDManager"
E2E = "ModAVD/ModAVDE2E"
VER = "0.42.0"
TAG = "v0.42.0"
URL = f"https://github.com/{REPO}/releases/tag/{TAG}"
BRANCH = "release/0.42"


def h(name):
    return hashlib.sha1(name.encode()).hexdigest()


class FakeGitHub:
    """Same methods as release_ci.Api, on in-memory data. Commits are global names -> sha; parents give ancestry."""

    def __init__(self):
        self.parents = {}      # sha -> [parent shas]
        self.trees = {}        # sha -> tree
        self.titles = {}       # sha -> first line
        self.refs = {}         # (repo, "heads/x" | "tags/x") -> sha
        self.pulls = {}        # (repo, n) -> dict
        self.pull_commit_list = {}   # (repo, n) -> [sha]
        self.commit_pull_list = {}   # sha -> [pull dicts]
        self.issues = {}       # n -> {"state", "title", "labels"}
        self.comments = {}     # n -> [bodies]
        self.files = {}        # (repo, path, sha) -> bytes
        self.releases = {}     # (repo, tag) -> dict
        self.calls = []        # mutations, in order
        self.fail = set()      # (op, n) that raise

    # -- building
    def commit(self, repo, name, parents=(), tree=None, title=None):
        sha = h(name)
        self.parents[sha] = [h(p) for p in parents]
        self.trees[sha] = tree or h("tree " + name)
        self.titles[sha] = title or name
        return sha

    def ancestors(self, sha):
        seen, todo = set(), [sha]
        while todo:
            s = todo.pop()
            if s not in seen:
                seen.add(s)
                todo += self.parents.get(s, [])
        return seen

    def resolve(self, repo, ref):
        if ref in self.parents:
            return ref
        return self.refs.get((repo, f"heads/{ref}")) or self.refs.get((repo, f"tags/{ref}")) or ref

    # -- Api
    def ref_commit(self, repo, ref):
        return self.refs.get((repo, ref), "")

    def pull(self, repo, number):
        if (repo, number) not in self.pulls:
            raise ci.NotFound(f"{repo}#{number}")
        return self.pulls[(repo, number)]

    def compare_status(self, repo, base, head):
        b, hd = self.resolve(repo, base), self.resolve(repo, head)
        if b == hd:
            return "identical"
        if b in self.ancestors(hd):
            return "ahead"
        if hd in self.ancestors(b):
            return "behind"
        return "diverged"

    def commit_tree(self, repo, sha):
        return self.trees[sha]

    def commit_sha(self, repo, ref):
        full = [s for s in self.parents if s.startswith(ref)]
        if len(full) != 1:
            raise ci.NotFound(ref)
        return full[0]

    def main_commits_since(self, repo, since):
        main = self.refs.get((repo, "heads/main"))
        return [(s, self.titles[s]) for s in self.ancestors(main)] if main else []

    def issue(self, repo, number):
        i = self.issues[number]
        return {"state": i["state"], "title": i["title"], "labels": list(i["labels"])}

    def issue_comments(self, repo, number):
        return list(self.comments.get(number, []))

    def file_bytes(self, repo, path, ref):
        return self.files.get((repo, path, ref))

    def release_by_tag(self, repo, tag):
        return self.releases.get((repo, tag))

    def commit_pulls(self, repo, sha):
        return self.commit_pull_list.get(sha, [])

    def pull_commits(self, repo, number):
        return self.pull_commit_list.get((repo, number), [])

    def _mutate(self, op, number):
        self.calls.append((op, number))
        if (op, number) in self.fail:
            raise RuntimeError(f"HTTP 502 on {op} #{number}")

    def comment(self, repo, number, body):
        self._mutate("comment", number)
        self.comments.setdefault(number, []).append(body)

    def close_issue(self, repo, number):
        self._mutate("close", number)
        self.issues[number]["state"] = "closed"

    def remove_label(self, repo, number, label):
        self._mutate("unlabel", number)
        assert label == ci.PENDING
        if label in self.issues[number]["labels"]:
            self.issues[number]["labels"].remove(label)


def entry(issue, prs, kind="close", note=""):
    return {"issue": issue, "prs": prs, "kind": kind, "note": note}


def merged(number, sha, base="main"):
    return {"number": number, "state": "closed", "merged_at": "2026-11-10T10:00:00Z", "merge_commit_sha": sha, "base": {"ref": base}, "body": ""}


class Scenario:
    """ModAVD main: m0 - p180 - p93 - m1 - p181 (merged after the freeze); release/0.42 = m1 - rel (tagged v0.42.0).
    ModAVDManager: g0 - p130 - gtag (v0.42.0) - p131. #93's recorded merge commit was rewritten off main (11-04)."""

    def __init__(self):
        g = self.gh = FakeGitHub()
        g.commit(REPO, "m0")
        self.p180 = g.commit(REPO, "p180", ["m0"], title="fix a (#180)")
        self.old93 = g.commit(REPO, "old93", ["m0"], tree=h("tree93"), title="乱码 (#93)")
        self.p93 = g.commit(REPO, "p93", ["p180"], tree=h("tree93"), title="fix b (#93)")
        self.m1 = g.commit(REPO, "m1", ["p93"])
        self.p181 = g.commit(REPO, "p181", ["m1"], title="fix c (#181)")
        self.test182 = g.commit(REPO, "test182", ["m0"])
        self.rel = g.commit(REPO, "rel", ["m1"])
        g.refs[(REPO, "heads/main")] = self.p181
        g.refs[(REPO, f"heads/{BRANCH}")] = self.rel
        g.refs[(REPO, f"tags/{TAG}")] = self.rel
        g.commit(MGR, "g0")
        self.p130 = g.commit(MGR, "p130", ["g0"])
        self.gtag = g.commit(MGR, "gtag", ["p130"])
        self.p131 = g.commit(MGR, "p131", ["gtag"])
        g.refs[(MGR, "heads/main")] = self.p131
        g.refs[(MGR, f"tags/{TAG}")] = self.gtag
        self.e40 = g.commit(E2E, "e40")
        g.refs[(E2E, "heads/main")] = self.e40
        g.pulls[(REPO, 180)] = merged(180, self.p180)
        g.pulls[(REPO, 181)] = merged(181, self.p181)
        g.pulls[(REPO, 93)] = merged(93, self.old93)
        # an open PR: GitHub still fills merge_commit_sha with its test merge
        g.pulls[(REPO, 182)] = {"number": 182, "state": "open", "merged_at": None, "merge_commit_sha": self.p180, "base": {"ref": "main"}}
        g.pulls[(MGR, 130)] = merged(130, self.p130)
        g.pulls[(MGR, 131)] = merged(131, self.p131)
        g.pulls[(E2E, 40)] = merged(40, self.e40)
        g.releases[(REPO, TAG)] = {"tag_name": TAG, "draft": False}
        self.entries = [
            entry(201, ["ModAVDManager#130"]),
            entry(202, ["ModAVD#180"], "keep", "Partly fixed in this release."),
            entry(203, ["ModAVDE2E#40"]),
            entry(204, ["ModAVD#181"]),
            entry(205, ["ModAVDManager#131"]),
            entry(206, [], "close", "qemu 补丁 0007 修的"),
            entry(207, ["ModAVD#182"]),
            entry(208, ["ModAVD#180"], "comment"),
            entry(209, ["ModAVD#180"]),
            entry(210, ["ModAVD#93"]),
            entry(211, ["ModAVDFoo#1"]),
            entry(212, [f"ModAVD@{self.p180[:12]}"]),
            entry(213, ["ModAVD#180", "ModAVD#181"]),
            entry(214, ["ModAVD#180"], "comment"),
        ]
        for n in range(201, 215):
            g.issues[n] = {"state": "open", "title": f"Issue {n}", "labels": [ci.PENDING]}
        g.issues[203]["state"] = "closed"
        g.issues[214]["state"] = "closed"
        g.issues[206]["labels"] = []      # nobody put the label on it
        g.issues[209]["state"] = "closed"
        g.comments[209] = [f"随 ModAVD {TAG} 发布：{URL}"]   # done last time, only the label is left
        self.write()

    def write(self, data=None):
        data = data if data is not None else {"version": VER, "manager": VER, "issues": self.entries}
        self.gh.files[(REPO, f"release/{VER}.issues.json", self.rel)] = ci.dump_issues(data).encode("utf-8")

    def run(self, dry_run=False):
        return ci.close_issues(self.gh, REPO, TAG, dry_run)


class CloseIssues(unittest.TestCase):
    CLOSED = [201, 206, 210, 212]            # kind close, every fix in this release
    COMMENTED = [202, 203, 214]              # keep (left open), already closed
    SKIPPED = [204, 205, 207, 208, 211, 213]

    def test_first_run(self):
        s = Scenario()
        code, lines = s.run()
        self.assertEqual(code, 0, "\n".join(lines))
        g = s.gh
        want = []
        for n in sorted(self.CLOSED + self.COMMENTED + [209]):
            if n in self.CLOSED:
                want += [("comment", n), ("close", n)]
            elif n in self.COMMENTED:
                want += [("comment", n)]
            if n != 206:
                want += [("unlabel", n)]
        self.assertEqual(g.calls, want)
        for n in self.CLOSED + [203, 209, 214]:
            self.assertEqual(g.issues[n]["state"], "closed", n)
        self.assertEqual(g.issues[202]["state"], "open")
        for n in self.SKIPPED:
            self.assertEqual(g.issues[n], {"state": "open", "title": f"Issue {n}", "labels": [ci.PENDING]}, n)
            self.assertNotIn(n, g.comments)
        self.assertEqual(g.comments[201], [f"随 ModAVD {TAG} 发布：{URL}"])
        self.assertEqual(g.comments[206], [f"随 ModAVD {TAG} 发布：{URL}\n\nqemu 补丁 0007 修的"])
        self.assertEqual(g.comments[202], [f"Partly fixed in this release.\n\n发布页：{URL}"])
        # kind close on an issue someone already closed: the same text as a close (as the local script did)
        self.assertEqual(g.comments[203], [f"随 ModAVD {TAG} 发布：{URL}"])
        self.assertEqual(g.comments[214], [f"已随 ModAVD {TAG} 发布：{URL}"])
        self.assertEqual(g.comments[209], [f"随 ModAVD {TAG} 发布：{URL}"])
        text = "\n".join(lines)
        for n, why in [(204, "ModAVD#181 的合并提交"), (205, "不在 v0.42.0 里"), (207, "ModAVD#182 还没合"),
                       (208, "还开着"), (211, "不认识的仓库 ModAVDFoo"), (213, "ModAVD#181 的合并提交")]:
            row = [line for line in text.split("\n") if line.startswith(f"| #{n} ")]
            self.assertTrue(row and why in row[-1] and "跳过" in row[-1], f"#{n}: {row}")
        self.assertIn("跳过 6 个", text)
        self.assertIn("改按 main 上的 squash 提交", text)

    def test_run_again_changes_nothing(self):
        s = Scenario()
        self.assertEqual(s.run()[0], 0)
        before = json.dumps([s.gh.issues, s.gh.comments], sort_keys=True)
        s.gh.calls.clear()
        code, lines = s.run()
        self.assertEqual(code, 0, "\n".join(lines))
        self.assertEqual(s.gh.calls, [])
        self.assertEqual(json.dumps([s.gh.issues, s.gh.comments], sort_keys=True), before)

    def test_failed_issue_keeps_label_and_run_again_finishes(self):
        s = Scenario()
        s.gh.fail = {("comment", 201), ("close", 210)}
        code, lines = s.run()
        self.assertEqual(code, 1)
        self.assertIn("还没做完：#201、#210", "\n".join(lines))
        self.assertEqual(s.gh.issues[201], {"state": "open", "title": "Issue 201", "labels": [ci.PENDING]})
        # 210's comment went out, its close did not: the label stays, and the run again does not comment twice
        self.assertEqual(s.gh.issues[210]["labels"], [ci.PENDING])
        self.assertEqual(len(s.gh.comments[210]), 1)
        self.assertEqual(s.gh.issues[212]["state"], "closed")   # the others went on
        s.gh.fail = set()
        s.gh.calls.clear()
        code, lines = s.run()
        self.assertEqual(code, 0, "\n".join(lines))
        self.assertEqual(s.gh.calls, [("comment", 201), ("close", 201), ("unlabel", 201), ("close", 210), ("unlabel", 210)])
        self.assertEqual(len(s.gh.comments[210]), 1)

    def test_dry_run_changes_nothing(self):
        s = Scenario()
        code, lines = s.run(dry_run=True)
        self.assertEqual((code, s.gh.calls), (0, []))
        self.assertIn("| #201 Issue 201 | ModAVDManager#130 | 关掉，附发布链接，去掉 !pending-release |", "\n".join(lines))

    def test_release_must_be_published(self):
        for release in [None, {"tag_name": TAG, "draft": True}]:
            s = Scenario()
            if release is None:
                del s.gh.releases[(REPO, TAG)]
            else:
                s.gh.releases[(REPO, TAG)] = release
            code, lines = s.run()
            self.assertEqual((code, s.gh.calls), (1, []))
            self.assertIn("还没发布", lines[0])

    def test_tag_and_issues_file_required(self):
        s = Scenario()
        self.assertEqual(ci.close_issues(s.gh, REPO, "v37.2.10-mas32")[0], 1)
        del s.gh.files[(REPO, f"release/{VER}.issues.json", s.rel)]
        code, lines = s.run()
        self.assertEqual((code, s.gh.calls), (1, []))
        self.assertIn("没有 release/0.42.0.issues.json", lines[0])
        s.write({"version": "0.41.1", "manager": VER, "issues": []})
        code, lines = s.run()
        self.assertEqual((code, s.gh.calls), (1, []))
        self.assertIn("not 0.42.0", lines[0])

    def test_issues_file_from_the_tagged_commit(self):
        """A different issues.json at the release branch head (after the release) is not read."""
        s = Scenario()
        later = s.gh.commit(REPO, "later", ["rel"])
        s.gh.refs[(REPO, f"heads/{BRANCH}")] = later
        s.gh.files[(REPO, f"release/{VER}.issues.json", later)] = ci.dump_issues({"version": VER, "manager": VER, "issues": [entry(204, ["ModAVD#181"])]}).encode()
        s.entries = [entry(201, ["ModAVDManager#130"])]
        s.write()
        self.assertEqual(s.run()[0], 0)
        self.assertEqual(s.gh.calls, [("comment", 201), ("close", 201), ("unlabel", 201)])

    def test_manager_without_tag_skips_its_prs(self):
        s = Scenario()
        del s.gh.refs[(MGR, f"tags/{TAG}")]
        code, lines = s.run()
        self.assertEqual(code, 0)
        self.assertNotIn(("close", 201), s.gh.calls)
        self.assertIn("ModAVD/ModAVDManager 还没有 v0.42.0", "\n".join(lines))

    def test_rewritten_merge_commit_needs_exactly_one_match(self):
        s = Scenario()
        s.gh.commit(REPO, "p93b", ["p93"], tree=h("tree93"), title="again (#93)")
        s.gh.parents[s.m1] = [h("p93b")]
        s.entries = [entry(210, ["ModAVD#93"])]
        s.write()
        code, lines = s.run()
        self.assertEqual((code, s.gh.calls), (0, []))
        self.assertIn("main 上有 2 个标题和树都对得上的提交", "\n".join(lines))
        s = Scenario()
        s.gh.trees[s.p93] = h("other tree")
        s.entries = [entry(210, ["ModAVD#93"])]
        s.write()
        code, lines = s.run()
        self.assertEqual((code, s.gh.calls), (0, []))
        self.assertIn("树不同", "\n".join(lines))

    def test_close_after_comment_only_closes(self):
        s = Scenario()
        s.gh.comments[201] = [f"随 ModAVD {TAG} 发布：{URL}"]
        s.entries = [entry(201, ["ModAVDManager#130"])]
        s.write()
        self.assertEqual(s.run()[0], 0)
        self.assertEqual(s.gh.calls, [("close", 201), ("unlabel", 201)])


class IssuesFile(unittest.TestCase):
    def good(self):
        return {"version": VER, "manager": "0.41.1", "issues": [entry(1, ["ModAVD#2", "ModAVDManager@abcdef1"]), entry(3, [], "keep", "x")]}

    def test_accepts_and_round_trips(self):
        data = self.good()
        self.assertEqual(ci.load_issues(json.loads(ci.dump_issues(data)), VER), data)
        self.assertTrue(ci.dump_issues(data).endswith("]\n}\n"))

    def test_refuses(self):
        cases = [
            (lambda d: d.update(version="0.42"), "version"),
            (lambda d: d.update(manager="v0.41.1"), "manager"),
            (lambda d: d.update(extra=1), "exactly"),
            (lambda d: d["issues"][0].update(kind="fix"), "kind"),
            (lambda d: d["issues"][0].update(issue="1"), "positive"),
            (lambda d: d["issues"][0].update(issue=True), "positive"),
            (lambda d: d["issues"][0].update(prs=["#2"]), "prs"),
            (lambda d: d["issues"][0].update(prs=["ModAVD#2 "]), "prs"),
            (lambda d: d["issues"][0].update(prs=["ModAVD#2", "ModAVD#2"]), "twice"),
            (lambda d: d["issues"][1].update(kind="close", note=" "), "source patch"),
            (lambda d: d["issues"][1].update(prs=["ModAVD#5"], note=""), "keep"),
            (lambda d: d["issues"].append(entry(1, ["ModAVD#9"])), "twice"),
            (lambda d: d["issues"][0].pop("note"), "exactly"),
        ]
        for change, message in cases:
            data = self.good()
            change(data)
            with self.assertRaisesRegex(ValueError, message):
                ci.load_issues(data)
        with self.assertRaisesRegex(ValueError, "not 0.42.1"):
            ci.load_issues(self.good(), "0.42.1")

    def test_command_writes_canonical_file(self):
        with tempfile.TemporaryDirectory() as d:
            src, out = Path(d) / "in.json", Path(d) / "release" / f"{VER}.issues.json"
            src.write_text(json.dumps([entry(7, ["ModAVD#8"], "close", "说明")]), encoding="utf-8")
            with mock.patch("sys.stdout", io.StringIO()):
                self.assertEqual(ci.main(["issues-json", "--version", VER, "--manager", VER, "--input", str(src), "--output", str(out)]), 0)
            self.assertEqual(out.read_bytes(), ci.dump_issues({"version": VER, "manager": VER, "issues": [entry(7, ["ModAVD#8"], "close", "说明")]}).encode())
            self.assertNotIn(b"\r", out.read_bytes())
            src.write_text(json.dumps([entry(7, ["nope"])]), encoding="utf-8")
            with mock.patch("sys.stderr", io.StringIO()):
                self.assertEqual(ci.main(["issues-json", "--version", VER, "--manager", VER, "--input", str(src), "--output", str(out) + "2"]), 1)
            self.assertFalse(Path(str(out) + "2").exists())


class ReviewedHead(unittest.TestCase):
    A, B = "a" * 40, "b" * 40

    def test_append_and_replace(self):
        self.assertEqual(ci.set_reviewed_head("发版 0.42.0。\n\nRefs ModAVD/ModAVD#1\n", self.A), f"发版 0.42.0。\n\nRefs ModAVD/ModAVD#1\n\nReviewed-Head: {self.A}\n")
        body = f"x\r\nReviewed-Head: {self.A}\r\ny\r\nReviewed-Head: {self.B}\r\n"
        self.assertEqual(ci.set_reviewed_head(body, self.B), f"x\nReviewed-Head: {self.B}\ny\n")
        self.assertEqual(ci.reviewed_heads(ci.set_reviewed_head("", self.A)), [self.A])
        with self.assertRaises(ValueError):
            ci.set_reviewed_head("x", "A" * 40)


class VerifyScenario:
    def __init__(self, target="sha"):
        g = self.gh = FakeGitHub()
        g.commit(REPO, "m0")
        self.head = g.commit(REPO, "prhead", ["m0"], tree=h("reltree"))
        self.rel = g.commit(REPO, "rel", ["m0"], tree=h("reltree"))
        g.refs[(REPO, f"heads/{BRANCH}")] = self.rel
        self.main_bytes, self.src_bytes = b"main zip bytes", b"source zip bytes"
        self.main, self.src = f"ModAVD-{VER}-win-x64.zip", f"ModAVD-{VER}-sources.zip"
        self.sums = f"{hashlib.sha256(self.main_bytes).hexdigest()}  {self.main}\n{hashlib.sha256(self.src_bytes).hexdigest()}  {self.src}\n".encode()
        self.notes = "# ModAVD 0.42.0\n\n公告\n\n<!-- modavd:github-only -->\n\n哈希\n"
        g.files[(REPO, f"release/{VER}.SHA256SUMS.txt", self.rel)] = self.sums
        g.files[(REPO, f"release/{VER}.md", self.rel)] = self.notes.encode()
        pr = {"number": 250, "merged_at": "2026-11-17T10:00:00Z", "merge_commit_sha": self.rel, "base": {"ref": BRANCH},
              "body": f"发版 ModAVD 0.42.0。\r\n\r\nReviewed-Head: {self.head}\r\n\r\nRefs ModAVD/ModAVD#1\r\n"}
        g.pulls[(REPO, 250)] = pr
        g.commit_pull_list[self.rel] = [pr]
        g.pull_commit_list[(REPO, 250)] = [h("prfirst"), self.head]
        # the asset is the packaging output's CRLF copy of the committed LF file
        self.files = {self.main: self.main_bytes, self.src: self.src_bytes, "SHA256SUMS.txt": self.sums.replace(b"\n", b"\r\n")}
        self.release = {"tag_name": TAG, "name": f"ModAVD {VER}", "draft": True, "html_url": "https://example.invalid/r",
                        "target_commitish": self.rel if target == "sha" else BRANCH, "body": self.notes.replace("\n", "\r\n") + "\r\n",
                        "assets": [{"name": n, "size": len(b), "digest": f"sha256:{hashlib.sha256(b).hexdigest()}"} for n, b in self.files.items()]}

    def downloaded(self):
        return "".join(f"{hashlib.sha256(b).hexdigest()}  {n}\n" for n, b in self.files.items())

    def run(self):
        return ci.verify_release(self.gh, REPO, VER, self.release, self.downloaded(), self.files.get("SHA256SUMS.txt"))


class VerifyRelease(unittest.TestCase):
    def check(self, s, *needles):
        problems, info = s.run()
        if not needles:
            self.assertEqual(problems, [], problems)
            return info
        text = "\n".join(problems)
        for n in needles:
            self.assertIn(n, text)
        return problems

    def test_good_draft(self):
        info = self.check(VerifyScenario())
        self.assertIn(f"#250 的合并提交和审过的头 `{VerifyScenario().head[:12]}` 树相同", info)
        self.check(VerifyScenario(target="branch"))

    def test_published_with_tag(self):
        s = VerifyScenario()
        s.release["draft"] = False
        s.gh.refs[(REPO, f"tags/{TAG}")] = s.rel
        self.check(s)
        s.gh.refs[(REPO, f"tags/{TAG}")] = s.head
        self.check(s, "已经在")

    def test_downloaded_asset_differs(self):
        s = VerifyScenario()
        s.files[s.main] = b"tampered"
        s.release["assets"][0]["digest"] = f"sha256:{hashlib.sha256(b'tampered').hexdigest()}"
        self.check(s, f"附件 {s.main} 下载下来是")

    def test_github_digest_differs(self):
        s = VerifyScenario()
        s.release["assets"][1]["digest"] = "sha256:" + "0" * 64
        self.check(s, "GitHub 摘要")

    def test_sums_asset_differs_from_commit(self):
        s = VerifyScenario()
        s.files["SHA256SUMS.txt"] = s.sums.replace(s.sums[:4], b"0000", 1)
        s.release["assets"][2]["digest"] = f"sha256:{hashlib.sha256(s.files['SHA256SUMS.txt']).hexdigest()}"
        self.check(s, "附件 SHA256SUMS.txt 的内容")
        s = VerifyScenario()
        s.files["SHA256SUMS.txt"] += b"x"   # the GitHub digest still names the original bytes
        self.check(s, "附件 SHA256SUMS.txt 的 GitHub 摘要")

    def test_asset_set(self):
        s = VerifyScenario()
        s.files["extra.txt"] = b"x"
        s.release["assets"].append({"name": "extra.txt", "digest": None})
        self.check(s, "附件应该正好是")
        s = VerifyScenario()
        del s.files[s.src]
        s.release["assets"] = [a for a in s.release["assets"] if a["name"] != s.src]
        self.check(s, "附件应该正好是")
        s = VerifyScenario()
        del s.files[s.src]
        self.check(s, "下载到的")

    def test_sums_file_must_list_the_two_zips(self):
        s = VerifyScenario()
        s.sums = s.sums.split(b"\n")[0] + b"\n"
        s.gh.files[(REPO, f"release/{VER}.SHA256SUMS.txt", s.rel)] = s.sums
        s.files["SHA256SUMS.txt"] = s.sums
        s.release["assets"][2]["digest"] = None
        self.check(s, "应该正好列")

    def test_title_tag_notes(self):
        s = VerifyScenario()
        s.release["name"] = "ModAVD 0.42"
        self.check(s, "标题")
        s = VerifyScenario()
        s.release["body"] += "多一行"
        self.check(s, "Release 说明")

    def test_commit_must_be_on_the_release_branch(self):
        s = VerifyScenario()
        other = s.gh.commit(REPO, "other", ["m0"], tree=h("reltree"))
        s.release["target_commitish"] = other
        self.check(s, "不在 release/0.42 上")
        s = VerifyScenario()
        s.release["target_commitish"] = "main"
        self.check(s, "既不是提交也不是")

    def test_release_pr(self):
        s = VerifyScenario()
        s.gh.commit_pull_list[s.rel] = []
        self.check(s, "找到 0 个")
        s = VerifyScenario()
        s.gh.commit_pull_list[s.rel][0]["base"] = {"ref": "main"}
        self.check(s, "找到 0 个")
        s = VerifyScenario()
        # the release commit is only one of the PR's commits, not its merge commit
        s.gh.commit_pull_list[s.rel][0]["merge_commit_sha"] = h("someothermerge")
        self.check(s, "找到 0 个")
        s = VerifyScenario()
        s.gh.pulls[(REPO, 250)]["body"] = "no line"
        self.check(s, "现在 0 行")
        s = VerifyScenario()
        s.gh.pulls[(REPO, 250)]["body"] += f"Reviewed-Head: {s.head}\n"
        self.check(s, "现在 2 行")
        s = VerifyScenario()
        s.gh.pulls[(REPO, 250)]["body"] = f"Reviewed-Head: {s.head[:12]}"
        self.check(s, "正好有一行")

    def test_reviewed_head_must_be_in_the_pr_with_the_same_tree(self):
        s = VerifyScenario()
        s.gh.trees[s.head] = h("older tree")
        self.check(s, "内容和审过的头")
        s = VerifyScenario()
        s.gh.pull_commit_list[(REPO, 250)] = [h("prfirst")]
        self.check(s, "不是这个 PR 的提交")

    def test_command_line(self):
        s = VerifyScenario()
        with tempfile.TemporaryDirectory() as d:
            rel, down, summary = Path(d) / "release.json", Path(d) / "sums.txt", Path(d) / "summary.md"
            rel.write_text(json.dumps(s.release), encoding="utf-8")
            down.write_text(s.downloaded(), encoding="utf-8")
            asset = Path(d) / "asset.txt"
            asset.write_bytes(s.files["SHA256SUMS.txt"])
            argv = ["verify-release", "--repo", REPO, "--version", VER, "--release-json", str(rel), "--downloaded", str(down),
                    "--sums-asset", str(asset), "--summary", str(summary)]
            with mock.patch("sys.stdout", io.StringIO()):
                self.assertEqual(ci.main(argv, gh=s.gh), 0)
                self.assertIn("可以发布", summary.read_text(encoding="utf-8"))
                s.files[s.main] = b"x"
                down.write_text(s.downloaded(), encoding="utf-8")
                self.assertEqual(ci.main(argv, gh=s.gh), 1)
                self.assertIn("不能发布", summary.read_text(encoding="utf-8"))
                down.write_text(VerifyScenario().downloaded(), encoding="utf-8")
                self.assertEqual(ci.main(argv, gh=s.gh), 0)
                asset.unlink()   # no SHA256SUMS.txt asset was downloaded
                summary.unlink()
                self.assertEqual(ci.main(argv, gh=s.gh), 1)
            self.assertIn("附件 SHA256SUMS.txt 的内容", summary.read_text(encoding="utf-8"))


class Pagination(unittest.TestCase):
    def test_next_link_must_stay_on_the_api(self):
        api = ci.Api("t", base="https://api.example.invalid")
        pages = iter([(b"[1]", {"Link": '<https://api.example.invalid/x?page=2>; rel="next"'}), (b"[2]", {"Link": '<https://evil.invalid/x?page=3>; rel="next"'})])
        with mock.patch.object(api, "_call", side_effect=lambda *a, **k: next(pages)):
            with self.assertRaisesRegex(RuntimeError, "left the API"):
                api.pages("x")


if __name__ == "__main__":
    unittest.main()
