"""Release steps that run on GitHub Actions (ModAVD/ModAVD#134). Standard library only.

  verify-release  .github/workflows/verify-release.yml: the (draft) release's downloaded assets against
                  release/<version>.SHA256SUMS.txt on the release commit, the release notes against release/<version>.md,
                  the release commit on release/0.<N>, and the release PR's merge commit having the same tree as the
                  head its body names on the Reviewed-Head line.
  close-issues    .github/workflows/close-released-issues.yml: after a release is published, go through
                  release/<version>.issues.json on the tagged commit; for every issue whose fixing PRs are in this
                  release, close it (or comment), link the release, remove !pending-release. Issues that do not check
                  out are skipped and listed. Running it again does nothing more than what is still missing.
  issues-json     write release/<version>.issues.json (tools/pubrel/release-pr.ps1 calls it from the publish plan).
  reviewed-head   put the Reviewed-Head line into a release PR body file (tools/pubrel/release-pr.ps1).

The GitHub calls go through Api (REST, token from GH_TOKEN); the tests give the functions a fake with the same methods.
The checks of close-issues are the ones the maintainer's local publish script made in its step 7.
"""
import argparse
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import urllib.error
import urllib.parse
import urllib.request

OWNER = "ModAVD"
VERSION_RE = re.compile(r"0\.([1-9][0-9]*)\.(0|[1-9][0-9]*)")
COMPONENT_VERSION_RE = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")
SHA_RE = re.compile(r"[0-9a-f]{40}")
# a fixing PR: "<repository>#<number>", or "<repository>@<commit>" when the commit itself is to be checked
PR_RE = re.compile(r"(ModAVD[A-Za-z0-9]*)(?:#([1-9][0-9]*)|@([0-9a-f]{7,40}))")
KINDS = ("close", "comment", "keep")
PENDING = "!pending-release"
# the release PR body names the head that was reviewed on a line of its own, exactly once
REVIEWED_PREFIX = "Reviewed-Head: "
REVIEWED_RE = re.compile(r"^Reviewed-Head:[ \t]*(\S*)[ \t]*$", re.M)
# tool repositories: their PRs are not shipped in the package, merged is enough
TOOL_REPOS = ("ModAVDE2E", "ModAVDHostIME")
ISSUE_KEYS = ("issue", "prs", "kind", "note")


class NotFound(Exception):
    pass


class Api:
    """The few REST calls the two workflows need."""

    def __init__(self, token, base="https://api.github.com"):
        if not token:
            raise ValueError("GH_TOKEN is not set")
        self.token = token
        self.base = base.rstrip("/")

    def _call(self, method, url, body=None, accept="application/vnd.github+json"):
        if not url.startswith(self.base + "/"):
            url = f"{self.base}/{url}"
        headers = {"Authorization": f"Bearer {self.token}", "Accept": accept, "X-GitHub-Api-Version": "2022-11-28",
                   "User-Agent": "modavd-release-ci"}
        data = None
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        request = urllib.request.Request(url, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=120) as response:
                return response.read(), response.headers
        except urllib.error.HTTPError as e:
            if e.code == 404:
                raise NotFound(f"{method} {url}: 404") from None
            raise RuntimeError(f"{method} {url}: HTTP {e.code} {e.read()[:300]!r}") from None

    def get(self, path):
        return json.loads(self._call("GET", path)[0])

    def pages(self, path):
        url = f"{path}{'&' if '?' in path else '?'}per_page=100"
        out = []
        while url:
            raw, headers = self._call("GET", url)
            out.extend(json.loads(raw))
            found = re.search(r'<([^>]+)>;\s*rel="next"', headers.get("Link") or "")
            url = found.group(1) if found else None
            # never send the token anywhere but the API
            if url and not url.startswith(self.base + "/"):
                raise RuntimeError(f"pagination left the API: {url}")
        return out

    def ref_commit(self, repo, ref):
        """The commit a ref ("tags/v0.42.0", "heads/release/0.42") points to, annotated tags peeled; '' if none."""
        try:
            obj = self.get(f"repos/{repo}/git/ref/{urllib.parse.quote(ref, safe='/')}")["object"]
        except NotFound:
            return ""
        for _ in range(5):
            if obj["type"] == "commit":
                return obj["sha"]
            if obj["type"] != "tag":
                raise ValueError(f"{repo} {ref} points to a {obj['type']}")
            obj = self.get(f"repos/{repo}/git/tags/{obj['sha']}")["object"]
        raise ValueError(f"{repo} {ref}: too many nested tags")

    def pull(self, repo, number):
        return self.get(f"repos/{repo}/pulls/{number}")

    def compare_status(self, repo, base, head):
        """'ahead' or 'identical' when base is an ancestor of head (or the same)."""
        return self.get(f"repos/{repo}/compare/{base}...{head}?per_page=1")["status"]

    def commit_tree(self, repo, sha):
        return self.get(f"repos/{repo}/git/commits/{sha}")["tree"]["sha"]

    def commit_sha(self, repo, ref):
        return self.get(f"repos/{repo}/commits/{ref}")["sha"]

    def main_commits_since(self, repo, since):
        return [(c["sha"], c["commit"]["message"].split("\n")[0]) for c in self.pages(f"repos/{repo}/commits?sha=main&since={since}")]

    def issue(self, repo, number):
        d = self.get(f"repos/{repo}/issues/{number}")
        return {"state": d["state"], "title": d.get("title") or "", "labels": [label["name"] for label in d.get("labels") or []]}

    def issue_comments(self, repo, number):
        return [c.get("body") or "" for c in self.pages(f"repos/{repo}/issues/{number}/comments")]

    def file_bytes(self, repo, path, ref):
        try:
            return self._call("GET", f"repos/{repo}/contents/{urllib.parse.quote(path)}?ref={ref}", accept="application/vnd.github.raw")[0]
        except NotFound:
            return None

    def release_by_tag(self, repo, tag):
        try:
            return self.get(f"repos/{repo}/releases/tags/{urllib.parse.quote(tag)}")
        except NotFound:
            return None

    def commit_pulls(self, repo, sha):
        return self.get(f"repos/{repo}/commits/{sha}/pulls")

    def pull_commits(self, repo, number):
        return [c["sha"] for c in self.pages(f"repos/{repo}/pulls/{number}/commits")]

    def comment(self, repo, number, body):
        self._call("POST", f"repos/{repo}/issues/{number}/comments", {"body": body})

    def close_issue(self, repo, number):
        self._call("PATCH", f"repos/{repo}/issues/{number}", {"state": "closed", "state_reason": "completed"})

    def remove_label(self, repo, number, label):
        try:
            self._call("DELETE", f"repos/{repo}/issues/{number}/labels/{urllib.parse.quote(label, safe='')}")
        except NotFound:
            pass   # already gone


# ---------------------------------------------------------------- release/<version>.issues.json

def load_issues(data, version=None):
    """Check release/<version>.issues.json:
    {"version": "0.42.0", "manager": "0.42.0",
     "issues": [{"issue": 103, "prs": ["ModAVDManager#54"], "kind": "close", "note": ""}, ...]}
    prs: "<repository>#<number>" or "<repository>@<commit>"; empty for an emulator source patch, then note says which.
    kind: close (close it and link the release), comment (already closed: comment only), keep (comment, leave it open)."""
    if not isinstance(data, dict) or sorted(data) != ["issues", "manager", "version"]:
        raise ValueError("issues.json must be an object with exactly version, manager, issues")
    if not isinstance(data["version"], str) or not VERSION_RE.fullmatch(data["version"]):
        raise ValueError(f"issues.json version is not 0.N.P: {data['version']!r}")
    if version is not None and data["version"] != version:
        raise ValueError(f"issues.json is for {data['version']}, not {version}")
    if not isinstance(data["manager"], str) or not COMPONENT_VERSION_RE.fullmatch(data["manager"]):
        raise ValueError(f"issues.json manager is not X.Y.Z: {data['manager']!r}")
    if not isinstance(data["issues"], list):
        raise ValueError("issues.json issues must be a list")
    seen = set()
    for e in data["issues"]:
        if not isinstance(e, dict) or sorted(e) != sorted(ISSUE_KEYS):
            raise ValueError(f"issues.json entry must have exactly {', '.join(ISSUE_KEYS)}: {e!r}")
        n = e["issue"]
        if type(n) is not int or n <= 0:
            raise ValueError(f"issues.json issue is not a positive number: {n!r}")
        if n in seen:
            raise ValueError(f"issues.json lists #{n} twice")
        seen.add(n)
        if e["kind"] not in KINDS:
            raise ValueError(f"issues.json #{n}: kind must be one of {', '.join(KINDS)}, not {e['kind']!r}")
        if not isinstance(e["note"], str):
            raise ValueError(f"issues.json #{n}: note must be a string")
        if not isinstance(e["prs"], list) or any(not isinstance(p, str) or not PR_RE.fullmatch(p) for p in e["prs"]):
            raise ValueError(f"issues.json #{n}: prs must be a list of <repository>#<number> or <repository>@<commit>: {e['prs']!r}")
        if len(set(e["prs"])) != len(e["prs"]):
            raise ValueError(f"issues.json #{n}: a PR is listed twice")
        if not e["prs"] and not e["note"].strip():
            raise ValueError(f"issues.json #{n}: no PR, so the note has to say which source patch fixes it")
        if e["kind"] == "keep" and not e["note"].strip():
            raise ValueError(f"issues.json #{n}: keep posts the note as the comment, so it cannot be empty")
    return data


def dump_issues(data):
    return json.dumps(data, ensure_ascii=False, indent=2) + "\n"


# ---------------------------------------------------------------- close-issues

def release_url(repo, tag):
    return f"https://github.com/{repo}/releases/tag/{tag}"


def resolve_main_commit(gh, repo, number, merged, merged_at):
    """The commit on main for a PR whose recorded merge commit is `merged`. When `merged` is not on main (main was
    rewritten, as on 11-04), the one commit on main whose title ends with " (#number)" and whose tree is `merged`'s.
    Returns (sha, '') or ('', why)."""
    status = gh.compare_status(repo, merged, "main")
    if status in ("ahead", "identical"):
        return merged, ""
    tree = gh.commit_tree(repo, merged)
    since = (datetime.fromisoformat(merged_at.replace("Z", "+00:00")) - timedelta(days=1)).astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    suffix = f" (#{number})"
    titled = [sha for sha, title in gh.main_commits_since(repo, since) if title.endswith(suffix)]
    same = [sha for sha in titled if gh.commit_tree(repo, sha) == tree]
    if len(same) == 1:
        return same[0], ""
    if len(same) > 1:
        why = f"main 上有 {len(same)} 个标题和树都对得上的提交（{'、'.join(s[:12] for s in same)}），人工核"
    elif titled:
        why = f"GitHub 记的合并提交 {merged[:12]} 不在 main 上（{status}），main 上标题对得上的 {'、'.join(s[:12] for s in titled)} 树不同，人工核"
    else:
        why = f"GitHub 记的合并提交 {merged[:12]} 不在 main 上（{status}），main 上也没有标题以「{suffix}」结尾的提交"
    return "", f"{repo}#{number}：{why}"


def check_pr(gh, key, release_commit, manager_tag, manager_sha, notes):
    """'' when the fix `key` is in this release, else why not."""
    m = PR_RE.fullmatch(key)
    if not m:
        return f"认不出的写法「{key}」"
    name = m.group(1)
    repo = f"{OWNER}/{name}"
    if name not in ("ModAVDManager", "ModAVD") + TOOL_REPOS:
        return f"不认识的仓库 {name}"
    if m.group(3):
        try:
            commit = gh.commit_sha(repo, m.group(3))
        except NotFound:
            return f"{key}：{repo} 里没有这个提交"
    else:
        number = int(m.group(2))
        try:
            pr = gh.pull(repo, number)
        except NotFound:
            return f"{key}：没有这个 PR"
        if not pr.get("merged_at") or not pr.get("merge_commit_sha"):
            return f"{key} 还没合（{pr.get('state')}）"
        commit, why = resolve_main_commit(gh, repo, number, pr["merge_commit_sha"], pr["merged_at"])
        if not commit:
            return why
        if commit != pr["merge_commit_sha"]:
            notes.append(f"{key}：GitHub 记的合并提交 {pr['merge_commit_sha'][:12]} 不在 main 上，改按 main 上的 squash 提交 {commit[:12]} 核（标题以「 (#{number})」结尾、树相同）")
    if name == "ModAVDManager":
        if not manager_sha:
            return f"{repo} 还没有 {manager_tag}"
        status = gh.compare_status(repo, commit, manager_sha)
        return "" if status in ("ahead", "identical") else f"{key} 的合并提交 {commit[:12]} 不在 {manager_tag} 里（{status}）"
    if name == "ModAVD":
        status = gh.compare_status(repo, commit, release_commit)
        return "" if status in ("ahead", "identical") else f"{key} 的合并提交 {commit[:12]} 不在这一版的发版提交里（{status}）"
    return ""   # a tool repository: not shipped in the package, merged is enough


def plan_issues(gh, repo, data, tag, release_commit, notes):
    """What to do with each issue now. action: close / comment / done (the release link is already in a comment and
    nothing else is left) / skip (why says why). label: !pending-release is still on it and is to be removed."""
    url = release_url(repo, tag)
    manager_tag = "v" + data["manager"]
    manager_sha = gh.ref_commit(f"{OWNER}/ModAVDManager", f"tags/{manager_tag}")
    keys = sorted({k for e in data["issues"] for k in e["prs"]})
    pr_why = {k: check_pr(gh, k, release_commit, manager_tag, manager_sha, notes) for k in keys}
    plan = []
    for e in data["issues"]:
        issue = gh.issue(repo, e["issue"])
        closed = issue["state"] == "closed"
        posted = any(url in body for body in gh.issue_comments(repo, e["issue"]))
        if e["prs"]:
            why = "；".join(pr_why[k] for k in e["prs"] if pr_why[k])
        else:
            why = "" if e["note"].strip() else "没写 PR，也不是写了说明的源码补丁"
        if why:
            action = "skip"
        elif posted and (closed or e["kind"] == "keep"):
            action = "done"
        elif closed or e["kind"] == "keep":
            action = "comment"
        elif e["kind"] == "close":
            action = "close"
        else:
            action, why = "skip", "还开着（以为合并时已经被 Fixes 关了），不动"
        plan.append({"issue": e["issue"], "title": issue["title"], "kind": e["kind"], "note": e["note"], "prs": e["prs"],
                     "state": issue["state"], "action": action, "why": why, "posted": posted,
                     "label": PENDING in issue["labels"] and action != "skip"})
    return plan


def comment_body(x, tag, url):
    note = f"\n\n{x['note']}" if x["note"] else ""
    if x["kind"] == "close":
        return f"随 ModAVD {tag} 发布：{url}{note}"
    if x["kind"] == "comment":
        return f"已随 ModAVD {tag} 发布：{url}{note}"
    return f"{x['note']}\n\n发布页：{url}"


def apply_plan(gh, repo, plan, tag):
    """Carry out the plan. An issue whose close or comment fails keeps its label, so a run again picks it up."""
    url = release_url(repo, tag)
    failures = []
    for x in plan:
        if x["action"] == "skip":
            continue
        try:
            if x["action"] == "close":
                if not x["posted"]:
                    gh.comment(repo, x["issue"], comment_body(x, tag, url))
                gh.close_issue(repo, x["issue"])
            elif x["action"] == "comment":
                gh.comment(repo, x["issue"], comment_body(x, tag, url))
            if x["label"]:
                gh.remove_label(repo, x["issue"], PENDING)
        except Exception as e:   # noqa: BLE001 - report and go on with the other issues
            failures.append(f"#{x['issue']}：{e}")
    return failures


ACTION_TEXT = {"close": "关掉，附发布链接", "comment": "补一条评论", "done": "已经做过", "skip": "跳过"}


def md(text):
    return str(text).replace("\\", "\\\\").replace("|", "\\|").replace("\n", " ").replace("<", "&lt;")


def issue_table(plan):
    rows = ["| issue | 修它的 PR | 做什么 | 原因 |", "| --- | --- | --- | --- |"]
    for x in plan:
        what = ACTION_TEXT[x["action"]] + ("，去掉 !pending-release" if x["label"] else "")
        rows.append(f"| #{x['issue']} {md(x['title'])} | {md('、'.join(x['prs']) or '源码补丁')} | {what} | {md(x['why'])} |")
    return rows


def close_issues(gh, repo, tag, dry_run=False):
    """Returns (exit code, summary lines)."""
    m = re.fullmatch(r"v(" + VERSION_RE.pattern + r")", tag or "")
    if not m:
        return 1, [f"tag 不是 v0.N.P：{tag!r}"]
    version = m.group(1)
    release = gh.release_by_tag(repo, tag)
    if not release or release.get("draft"):
        return 1, [f"{repo} 的 {tag} 还没发布，不动 issue"]
    commit = gh.ref_commit(repo, f"tags/{tag}")
    if not commit:
        return 1, [f"{repo} 没有 tag {tag}"]
    raw = gh.file_bytes(repo, f"release/{version}.issues.json", commit)
    if raw is None:
        return 1, [f"{tag}（{commit[:12]}）上没有 release/{version}.issues.json：发版 PR 要带上它（tools/pubrel/release-pr.ps1 生成）"]
    try:
        data = load_issues(json.loads(raw.decode("utf-8")), version)
    except (ValueError, UnicodeDecodeError) as e:
        return 1, [f"release/{version}.issues.json 不对：{e}"]
    notes = []
    plan = plan_issues(gh, repo, data, tag, commit, notes)
    lines = [f"## 关 issue：ModAVD {tag}", "", f"发版提交 `{commit}`，管理器 v{data['manager']}，发布页 {release_url(repo, tag)}", ""]
    lines += [f"- 注意：{md(n)}" for n in notes]
    lines += ["", "### 这次要做的", ""] + issue_table(plan)
    if dry_run:
        return 0, lines + ["", "（--dry-run：什么都没改）"]
    failures = apply_plan(gh, repo, plan, tag)
    after = plan_issues(gh, repo, data, tag, commit, [])
    left = [x for x in after if x["action"] in ("close", "comment") or x["label"]]
    skipped = [x for x in after if x["action"] == "skip"]
    lines += ["", "### 做完以后", ""] + issue_table(after)
    if failures:
        lines += ["", "### 失败", ""] + [f"- {md(f)}" for f in failures]
    if skipped:
        lines += ["", f"跳过 {len(skipped)} 个（核不过，留着 !pending-release，下一版再核）：" + "、".join(f"#{x['issue']}" for x in skipped)]
    if left:
        lines += ["", "还没做完：" + "、".join(f"#{x['issue']}" for x in left) + "；重跑这个 workflow 会接着做"]
        return 1, lines
    return 0, lines


# ---------------------------------------------------------------- verify-release

def parse_sums(text):
    """'<sha256>  <name>' lines (sha256sum's '*' binary marker allowed) -> {name: sha256}."""
    out = {}
    for line in text.replace("\r\n", "\n").split("\n"):
        m = re.fullmatch(r"([0-9a-f]{64}) [ *](.+)", line)
        if m:
            if m.group(2) in out:
                raise ValueError(f"{m.group(2)} is listed twice")
            out[m.group(2)] = m.group(1)
    return out


def norm(text):
    """Release notes as compared: LF line ends, no trailing blanks, no trailing empty lines."""
    return "\n".join(line.rstrip() for line in text.replace("\r\n", "\n").split("\n")).rstrip("\n")


def reviewed_heads(body):
    return REVIEWED_RE.findall((body or "").replace("\r\n", "\n"))


def verify_release(gh, repo, version, release, downloaded, sums_asset):
    """Problems with a (draft) release, as a list of strings (empty = it may be published), and what was checked.
    release: the release as the API lists it; downloaded: sha256sum output of every downloaded asset;
    sums_asset: the bytes of the downloaded SHA256SUMS.txt asset (None if there was none)."""
    m = VERSION_RE.fullmatch(version or "")
    if not m:
        return [f"版本号不是 0.N.P：{version!r}"], []
    tag, branch = f"v{version}", f"release/0.{m.group(1)}"
    main_zip, src_zip, sums_name = f"ModAVD-{version}-win-x64.zip", f"ModAVD-{version}-sources.zip", "SHA256SUMS.txt"
    problems, info = [], []
    if release.get("tag_name") != tag:
        problems.append(f"Release 的 tag 是 {release.get('tag_name')!r}，不是 {tag}")
    if release.get("name") != f"ModAVD {version}":
        problems.append(f"Release 的标题是 {release.get('name')!r}，不是「ModAVD {version}」")
    info.append(f"Release {'草稿' if release.get('draft') else '已发布'}：{release.get('html_url', '')}")

    # the release commit: what the draft targets (a commit, or the release branch when release.ps1 -DraftRelease made it)
    target = release.get("target_commitish") or ""
    tag_sha = gh.ref_commit(repo, f"tags/{tag}")
    branch_head = gh.ref_commit(repo, f"heads/{branch}")
    if not branch_head:
        return problems + [f"{repo} 没有 {branch}"], info
    if SHA_RE.fullmatch(target):
        commit = target
    elif target == branch:
        commit = tag_sha or branch_head
    else:
        return problems + [f"Release 的目标 {target!r} 既不是提交也不是 {branch}"], info
    if tag_sha and tag_sha != commit:
        problems.append(f"{tag} 已经在 {tag_sha[:12]} 上，不是 Release 的目标 {commit[:12]}")
    status = gh.compare_status(repo, commit, branch)
    if status not in ("ahead", "identical"):
        problems.append(f"发版提交 {commit[:12]} 不在 {branch} 上（{status}）")
    info.append(f"发版提交 `{commit}`（{branch} 的头 `{branch_head[:12]}`）")

    # assets against release/<version>.SHA256SUMS.txt on that commit
    sums_bytes = gh.file_bytes(repo, f"release/{version}.SHA256SUMS.txt", commit)
    notes_bytes = gh.file_bytes(repo, f"release/{version}.md", commit)
    if sums_bytes is None or notes_bytes is None:
        return problems + [f"{commit[:12]} 上没有 release/{version}.SHA256SUMS.txt 或 release/{version}.md"], info
    listed = parse_sums(sums_bytes.decode("utf-8"))
    if sorted(listed) != sorted([main_zip, src_zip]):
        problems.append(f"release/{version}.SHA256SUMS.txt 应该正好列 {main_zip} 和 {src_zip}：{', '.join(listed) or '一行都没有'}")
    # the SHA256SUMS.txt asset is the packaging output's (CRLF), the committed one is LF: compare the lines
    if sums_asset is None or parse_sums(sums_asset.decode("utf-8", "replace")) != listed:
        problems.append(f"附件 SHA256SUMS.txt 的内容和 {commit[:12]} 上的 release/{version}.SHA256SUMS.txt 不一样")
    expected = {main_zip: listed.get(main_zip), src_zip: listed.get(src_zip),
                sums_name: hashlib.sha256(sums_asset).hexdigest() if sums_asset is not None else None}
    assets = {a["name"]: a for a in release.get("assets") or []}
    got = parse_sums(downloaded)
    if sorted(assets) != sorted(expected):
        problems.append(f"附件应该正好是 {', '.join(sorted(expected))}：现在是 {', '.join(sorted(assets)) or '没有'}")
    if sorted(got) != sorted(assets):
        problems.append(f"下载到的 {', '.join(sorted(got)) or '没有'} 和附件列表 {', '.join(sorted(assets)) or '没有'} 不一样")
    for name in sorted(expected):
        if name not in assets or name not in got:
            continue
        if not expected[name] or got[name] != expected[name]:
            where = "下载下来的 SHA256SUMS.txt 文件" if name == sums_name else "提交上的校验表"
            problems.append(f"附件 {name} 下载下来是 {got[name]}，{where}是 {expected[name] or '（没有）'}")
        digest = assets[name].get("digest")
        if digest and digest != f"sha256:{got[name]}":
            problems.append(f"附件 {name} 的 GitHub 摘要 {digest} 和下载下来的 {got[name]} 不一样")
        info.append(f"`{got[name]}`  {name}")
    if norm(release.get("body") or "") != norm(notes_bytes.decode("utf-8")):
        problems.append(f"Release 说明和 {commit[:12]} 上的 release/{version}.md 不一样")

    # the release PR: merged into the release branch as this commit; the reviewed head named in its body has this tree
    prs = [p for p in gh.commit_pulls(repo, commit)
           if p.get("merged_at") and p.get("merge_commit_sha") == commit and (p.get("base") or {}).get("ref") == branch]
    if len(prs) != 1:
        problems.append(f"找不到正好一个合并成 {commit[:12]}、合进 {branch} 的发版 PR（找到 {len(prs)} 个）")
        return problems, info
    number = prs[0]["number"]
    body = gh.pull(repo, number).get("body") or ""
    heads = reviewed_heads(body)
    if len(heads) != 1 or not SHA_RE.fullmatch(heads[0]):
        problems.append(f"#{number} 的正文要正好有一行「{REVIEWED_PREFIX}<40 位提交>」（tools/pubrel/release-pr.ps1 -BodyFile 生成）：现在 {len(heads)} 行")
        return problems, info
    head = heads[0]
    if head not in gh.pull_commits(repo, number):
        problems.append(f"#{number} 写的审过的头 {head[:12]} 不是这个 PR 的提交")
    elif gh.commit_tree(repo, head) != gh.commit_tree(repo, commit):
        problems.append(f"#{number} 的合并提交 {commit[:12]} 的内容和审过的头 {head[:12]} 不一样（审过以后又进了改动？）")
    else:
        info.append(f"#{number} 的合并提交和审过的头 `{head[:12]}` 树相同")
    return problems, info


# ---------------------------------------------------------------- reviewed-head

def set_reviewed_head(text, head):
    """The PR body with exactly one Reviewed-Head line naming `head` (replacing any there, else appended)."""
    if not SHA_RE.fullmatch(head or ""):
        raise ValueError(f"reviewed head must be 40 lowercase hex digits: {head!r}")
    line = REVIEWED_PREFIX + head
    out, placed = [], False
    for old in text.replace("\r\n", "\n").split("\n"):
        if REVIEWED_RE.fullmatch(old):
            if not placed:
                out.append(line)
                placed = True
            continue
        out.append(old)
    text = "\n".join(out).rstrip("\n")
    if not placed:
        text += ("\n\n" if text.strip() else "") + line
    return text + "\n"


# ---------------------------------------------------------------- command line

def write_summary(path, lines):
    text = "\n".join(lines) + "\n"
    sys.stdout.write(text)
    if path:
        with open(path, "a", encoding="utf-8") as f:
            f.write(text)


def main(argv=None, gh=None):
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")   # the summary is Chinese; Windows consoles are not UTF-8
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("verify-release")
    p.add_argument("--repo", required=True)
    p.add_argument("--version", required=True)
    p.add_argument("--release-json", required=True, help="the release object as the API returns it")
    p.add_argument("--downloaded", required=True, help="sha256sum output of the downloaded assets")
    p.add_argument("--sums-asset", required=True, help="the downloaded SHA256SUMS.txt asset (may be missing)")
    p.add_argument("--summary", default="")
    p = sub.add_parser("close-issues")
    p.add_argument("--repo", required=True)
    p.add_argument("--tag", required=True)
    p.add_argument("--summary", default="")
    p.add_argument("--dry-run", action="store_true")
    p = sub.add_parser("issues-json")
    p.add_argument("--version", required=True)
    p.add_argument("--manager", required=True)
    p.add_argument("--input", required=True, help="JSON list of {issue, prs, kind, note}")
    p.add_argument("--output", required=True)
    p = sub.add_parser("reviewed-head")
    p.add_argument("--body", required=True, help="PR body file, rewritten in place")
    p.add_argument("--head", required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "issues-json":
            items = json.loads(Path(args.input).read_text(encoding="utf-8"))
            data = load_issues({"version": args.version, "manager": args.manager, "issues": items})
            Path(args.output).parent.mkdir(parents=True, exist_ok=True)
            Path(args.output).write_bytes(dump_issues(data).encode("utf-8"))
            print(f"{args.output}: {len(data['issues'])} issues")
            return 0
        if args.command == "reviewed-head":
            path = Path(args.body)
            path.write_bytes(set_reviewed_head(path.read_text(encoding="utf-8"), args.head).encode("utf-8"))
            print(f"{path}: {REVIEWED_PREFIX}{args.head}")
            return 0
        gh = gh or Api(os.environ.get("GH_TOKEN", ""))
        if args.command == "close-issues":
            code, lines = close_issues(gh, args.repo, args.tag, args.dry_run)
            write_summary(args.summary, lines)
            return code
        release = json.loads(Path(args.release_json).read_text(encoding="utf-8"))
        downloaded = Path(args.downloaded).read_text(encoding="utf-8")
        sums_asset = Path(args.sums_asset).read_bytes() if Path(args.sums_asset).is_file() else None
        problems, info = verify_release(gh, args.repo, args.version, release, downloaded, sums_asset)
        lines = [f"## 核对 ModAVD {args.version} 的 Release", ""] + [f"- {md(i)}" for i in info]
        if problems:
            lines += ["", "### 对不上，不能发布", ""] + [f"- {md(x)}" for x in problems]
        else:
            lines += ["", "全部对上：附件 = 发版提交上的校验表，说明 = release 文件，合并提交 = 审过的头。可以发布。"]
        write_summary(args.summary, lines)
        return 1 if problems else 0
    except Exception as e:   # noqa: BLE001
        print(f"ERROR [{args.command}]: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
