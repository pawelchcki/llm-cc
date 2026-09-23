"""GitHub client and discovery tests against an in-memory REST API."""

import argparse
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

from .discover import fetch_target, run as discover_run, write_dotenv
from .fixtures import FakeGitHub, commit_files, git
from .github import GitHub, api_request

HEAD = "a" * 40
TARGET = "b" * 40


class DiscoveryTest(unittest.TestCase):
    def setUp(self):
        self.github = FakeGitHub()
        self.github.branches.update(main=TARGET, release="c" * 40)

    def test_default_branch_compares_the_head_with_itself(self):
        self.github.branches["main"] = HEAD
        self.assertEqual(
            self.github.discover(HEAD, "main", "main"),
            {
                "head_sha": HEAD,
                "target_sha": HEAD,
                "target_branch": "main",
                "pr_number": None,
            },
        )
        self.github.branches["main"] = TARGET
        with self.assertRaisesRegex(ValueError, "superseded"):
            self.github.discover(HEAD, "main", "main")

    def test_branch_without_open_pr_is_skipped(self):
        self.assertIsNone(self.github.discover(HEAD, "feature", "main"))
        self.github.open_pull(3, "feature", HEAD)["state"] = "closed"
        self.assertIsNone(self.github.discover(HEAD, "feature", "main"))

    def test_actual_target_is_resolved_even_off_the_default_branch(self):
        self.github.open_pull(7, "feature", HEAD, base_ref="release")
        self.assertEqual(
            self.github.discover(HEAD, "feature", "main"),
            {
                "head_sha": HEAD,
                "target_sha": "c" * 40,
                "target_branch": "release",
                "pr_number": 7,
            },
        )

    def test_ambiguous_pull_requests_are_rejected(self):
        self.github.open_pull(7, "feature", HEAD)
        self.github.open_pull(8, "feature", HEAD, base_ref="release")
        with self.assertRaisesRegex(ValueError, "multiple open PRs"):
            self.github.discover(HEAD, "feature", "main")

    def test_other_repositories_and_branches_are_filtered(self):
        # The same commit proposed to an upstream repository, or pushed under
        # another branch name, must not select this repository's comparison.
        self.github.open_pull(5, "feature", HEAD, base_repo="upstream/repo")
        self.github.open_pull(6, "other-branch", HEAD)
        self.assertIsNone(self.github.discover(HEAD, "feature", "main"))
        self.github.open_pull(7, "feature", HEAD)
        self.assertEqual(self.github.discover(HEAD, "feature", "main")["pr_number"], 7)

    def test_current_rechecks_state_head_and_target(self):
        pull = self.github.open_pull(7, "feature", HEAD)
        identity = self.github.discover(HEAD, "feature", "main")
        self.assertTrue(self.github.current(identity))
        for field, value in (
            (("head", "sha"), "d" * 40),
            (("base", "sha"), "e" * 40),
            (("base", "ref"), "release"),
        ):
            with self.subTest(field=field):
                previous = pull[field[0]][field[1]]
                pull[field[0]][field[1]] = value
                self.assertFalse(self.github.current(identity))
                pull[field[0]][field[1]] = previous
        pull["state"] = "closed"
        self.assertFalse(self.github.current(identity))


class CommentApiTest(unittest.TestCase):
    def test_comments_are_read_across_pages(self):
        github = FakeGitHub()
        for index in range(150):
            github.add_comment(9, "comment %d" % index, "someone")
        github.add_comment(10, "another pull request", "someone")
        comments = github.issue_comments(9)
        self.assertEqual(len(comments), 150)
        self.assertEqual(comments[-1]["body"], "comment 149")
        pages = [path for method, path in github.calls if "/comments?" in path]
        self.assertEqual(len(pages), 2)

    def test_writes_use_the_issue_comment_endpoints(self):
        github = GitHub("owner/repo", "token", "https://github.example/api/v3/")
        with mock.patch(
            "tools.comparison.github.api_request", return_value={"id": 5}
        ) as request:
            github.create_comment(9, "body")
            github.update_comment(5, "new body")
        self.assertEqual(
            request.call_args_list,
            [
                mock.call(
                    "https://github.example/api/v3/repos/owner/repo/issues/9/comments",
                    "token",
                    {"body": "body"},
                    method="POST",
                ),
                mock.call(
                    "https://github.example/api/v3/repos/owner/repo/issues/comments/5",
                    "token",
                    {"body": "new body"},
                    method="PATCH",
                ),
            ],
        )

    def test_request_sends_method_and_bearer_token(self):
        response = mock.MagicMock()
        response.__enter__.return_value.read.return_value = b""
        with mock.patch("urllib.request.urlopen", return_value=response) as urlopen:
            self.assertIsNone(
                api_request("https://api.example/x", "secret", {"a": 1}, method="PATCH")
            )
        request = urlopen.call_args.args[0]
        self.assertEqual(request.get_method(), "PATCH")
        self.assertEqual(request.get_header("Authorization"), "Bearer secret")
        self.assertEqual(request.data, b'{"a":1}')

    def test_repository_and_api_url_are_validated(self):
        with self.assertRaises(ValueError):
            GitHub("not a repository")
        with self.assertRaises(ValueError):
            GitHub("owner/repo", api_url="http://api.github.com")


class DiscoverCommandTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.github = FakeGitHub()
        self.github.branches["main"] = TARGET

    def args(self, **overrides):
        values = dict(
            repository="owner/repo",
            head=HEAD,
            branch="feature",
            default_branch="main",
            pipeline_id="1234",
            repo=None,
            output=str(self.root / "identity.json"),
            dotenv=str(self.root / "discover.env"),
        )
        values.update(overrides)
        return argparse.Namespace(**values)

    def dotenv(self):
        return dict(
            line.split("=", 1)
            for line in (self.root / "discover.env").read_text().splitlines()
        )

    def test_pull_request_identity_and_outputs(self):
        self.github.open_pull(7, "feature", HEAD)
        with mock.patch("sys.stdout", io.StringIO()):
            self.assertEqual(discover_run(self.args(), self.github), 0)
        identity = json.loads((self.root / "identity.json").read_text())
        self.assertEqual(
            {key: identity[key] for key in ("pipeline_id", "pr_number", "target_sha")},
            {"pipeline_id": "1234", "pr_number": 7, "target_sha": TARGET},
        )
        self.assertIsNone(identity["base_sha"])
        self.assertEqual(
            self.dotenv(),
            {
                "COMPARISON_SKIP": "0",
                "COMPARISON_PR_NUMBER": "7",
                "COMPARISON_TARGET_SHA": TARGET,
            },
        )

    def test_default_branch_outputs_omit_the_pr_number(self):
        self.github.branches["main"] = HEAD
        with mock.patch("sys.stdout", io.StringIO()):
            discover_run(self.args(branch="main"), self.github)
        self.assertEqual(
            self.dotenv(), {"COMPARISON_SKIP": "0", "COMPARISON_TARGET_SHA": HEAD}
        )

    def test_branch_without_pull_request_writes_a_skip(self):
        with mock.patch("sys.stdout", io.StringIO()):
            self.assertEqual(discover_run(self.args(), self.github), 0)
        self.assertFalse((self.root / "identity.json").exists())
        skipped = json.loads((self.root / "skipped.json").read_text())
        self.assertEqual(skipped["identity"]["head_sha"], HEAD)
        self.assertEqual(self.dotenv(), {"COMPARISON_SKIP": "1"})

    def test_abbreviated_heads_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "full"):
            discover_run(self.args(head="abc123"), self.github)

    def test_dotenv_appends_only_safe_values(self):
        path = self.root / "outputs"
        path.write_text("EXISTING=1\n")
        write_dotenv(path, {"A": "x/y-1.2", "B": None})
        self.assertEqual(path.read_text(), "EXISTING=1\nA=x/y-1.2\nB=\n")
        for name, value in (("A B", "x"), ("A", "x\nEVIL=1"), ("A", "$(id)")):
            with self.subTest(name=name, value=value):
                with self.assertRaises(ValueError):
                    write_dotenv(path, {name: value})


class FetchTargetTest(unittest.TestCase):
    def test_shallow_checkout_gains_the_target_and_full_history(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            origin = root / "origin"
            origin.mkdir()
            git(origin, "init", "-q", "-b", "main")
            target = commit_files(origin, {"a.cc": "int a;\n"}, "target")
            commit_files(origin, {"b.cc": "int b;\n"}, "later")
            git(origin, "checkout", "-q", "-b", "feature")
            head = commit_files(origin, {"c.cc": "int c;\n"}, "feature")
            checkout = root / "checkout"
            subprocess.run(
                [
                    "git",
                    "clone",
                    "-q",
                    "--depth",
                    "1",
                    "--branch",
                    "feature",
                    origin.as_uri(),
                    str(checkout),
                ],
                check=True,
            )
            self.assertEqual(
                git(checkout, "rev-parse", "--is-shallow-repository"), "true"
            )
            fetch_target(checkout, target, "main")
            self.assertEqual(
                git(checkout, "rev-parse", "--is-shallow-repository"), "false"
            )
            self.assertEqual(git(checkout, "merge-base", target, head), target)

    def test_mirror_refusing_object_fetches_falls_back_to_the_branch(self):
        calls = []

        def run(command, check=False):
            calls.append(command[3:])
            if command[3:5] == ["fetch", "origin"] and command[5] == TARGET:
                return subprocess.CompletedProcess(command, 128)
            return subprocess.CompletedProcess(command, 0)

        with (
            mock.patch("tools.comparison.discover.subprocess.run", side_effect=run),
            mock.patch(
                "tools.comparison.discover.subprocess.check_output",
                return_value="false\n",
            ),
        ):
            fetch_target("/repo", TARGET, "release")
        self.assertEqual(
            calls,
            [
                ["fetch", "origin", TARGET],
                ["fetch", "origin", "+refs/heads/release:refs/remotes/origin/release"],
                ["cat-file", "-e", TARGET + "^{commit}"],
            ],
        )


if __name__ == "__main__":
    unittest.main()
