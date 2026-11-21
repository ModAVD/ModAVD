import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

# CI runs this with python -I, which leaves the script's own directory off sys.path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import repo_checks  # noqa: E402


class RepoChecksTest(unittest.TestCase):
    def test_allowed_identities(self):
        self.assertTrue(repo_checks.allowed("qiyinxi", "qiyinxi@foxmail.com"))
        self.assertTrue(repo_checks.allowed("bot", "123+bot[bot]@users.noreply.github.com"))
        # Dependabot 的提交作者，原样照 GitHub 写的抄。
        self.assertTrue(repo_checks.allowed("dependabot[bot]", "49699333+dependabot[bot]@users.noreply.github.com"))
        self.assertTrue(repo_checks.allowed("someone", "someone@users.noreply.github.com"))
        self.assertFalse(repo_checks.allowed("ai", "noreply@openai.com"))
        self.assertFalse(repo_checks.allowed("Claude", "noreply@" + "anthropic.com"))
        self.assertFalse(repo_checks.allowed("qiyinxi", "someone@example.com"))
        self.assertTrue(repo_checks.allowed("Old", "octocat@users.noreply.github.com"))
        self.assertFalse(repo_checks.allowed("Someone", "someone@example.com"))
        self.assertFalse(repo_checks.allowed("Codex", "noreply@openai.com"))

    def test_commit_and_coauthor(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            subprocess.run(["git", "config", "user.name", "qiyinxi"], cwd=root, check=True)
            subprocess.run(["git", "config", "user.email", "qiyinxi@foxmail.com"], cwd=root, check=True)
            (root / "x").write_text("x")
            subprocess.run(["git", "add", "x"], cwd=root, check=True)
            env = dict(os.environ, GIT_AUTHOR_NAME="qiyinxi", GIT_AUTHOR_EMAIL="qiyinxi@foxmail.com")
            subprocess.run(["git", "commit", "-qm", "ok\n\nCo-authored-by: bot <123+bot[bot]@users.noreply.github.com>"], cwd=root, env=env, check=True)
            commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
            self.assertEqual(repo_checks.check_commit(commit, str(root)), [])

    def test_rejects_ai_trailer(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            subprocess.run(["git", "config", "user.name", "qiyinxi"], cwd=root, check=True)
            subprocess.run(["git", "config", "user.email", "qiyinxi@foxmail.com"], cwd=root, check=True)
            (root / "x").write_text("x")
            subprocess.run(["git", "add", "x"], cwd=root, check=True)
            subprocess.run(["git", "commit", "-qm", "bad\n\n  co-authored-by: bot <noreply@openai.com>"], cwd=root, check=True)
            commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
            self.assertTrue(repo_checks.check_commit(commit, str(root)))


if __name__ == "__main__":
    unittest.main()
