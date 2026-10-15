"""GitHub client and discovery tests against an in-memory REST API."""

import argparse
import email.message
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock
import urllib.error

from .discover import (
    Unresolved,
    branch_ref,
    current_from_git,
    discover_anonymous,
    fetch_target,
    new_identity,
    pull_from_git,
    run as discover_run,
    write_dotenv,
)
from .fixtures import FakeGitHub, commit_files, git
from .github import GitHub, RateLimited, api_request, rate_limit_delay

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
        # The same commit proposed to an upstream repository, pushed under
        # another branch name, or proposed from a fork's same-named branch,
        # must not select this repository's comparison.
        self.github.open_pull(5, "feature", HEAD, base_repo="upstream/repo")
        self.github.open_pull(6, "other-branch", HEAD)
        self.github.open_pull(8, "feature", HEAD, head_repo="fork/repo")
        self.assertIsNone(self.github.discover(HEAD, "feature", "main"))
        self.github.open_pull(7, "feature", HEAD)
        self.assertEqual(self.github.discover(HEAD, "feature", "main")["pr_number"], 7)

    def test_repository_names_match_in_any_case(self):
        # GitHub's canonical full_name may differ in case from the configured one.
        self.github.open_pull(
            7, "feature", HEAD, base_repo="Owner/Repo", head_repo="OWNER/repo"
        )
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


    def test_a_named_pull_request_is_read_once_and_filtered_alike(self):
        self.github.open_pull(7, "feature", HEAD)
        self.assertEqual(
            self.github.discover_pull(7, HEAD, "feature"),
            self.github.discover(HEAD, "feature", "main"),
        )
        self.github.calls.clear()
        self.github.discover_pull(7, HEAD, "feature")
        self.assertEqual(self.github.calls, [("GET", "/repos/owner/repo/pulls/7")])
        self.github.open_pull(8, "feature", HEAD, head_repo="fork/repo")
        for number, head, branch in ((8, HEAD, "feature"), (7, "d" * 40, "feature"),
                                     (7, HEAD, "other")):
            with self.subTest(number=number, head=head, branch=branch):
                self.assertIsNone(self.github.discover_pull(number, head, branch))


def limited(code=403, reason="rate limit exceeded", body=b"", **headers):
    message = email.message.Message()
    for name, value in headers.items():
        message[name.replace("_", "-")] = value
    return urllib.error.HTTPError(
        "https://api.github.com/x", code, reason, message, io.BytesIO(body)
    )


class RateLimitTest(unittest.TestCase):
    def setUp(self):
        self.errors = []

    def tearDown(self):
        for error in self.errors:
            error.close()

    def limited(self, *args, **kwargs):
        self.errors.append(limited(*args, **kwargs))
        return self.errors[-1]

    def test_primary_and_secondary_limits_name_their_wait(self):
        limited = self.limited
        primary = limited(x_ratelimit_remaining="0", x_ratelimit_reset="1060")
        self.assertEqual(rate_limit_delay(primary, now=1000), 61)
        self.assertEqual(rate_limit_delay(limited(429, retry_after="30"), now=0), 30)
        # A secondary limit with only a message waits GitHub's documented minute.
        secondary = limited(
            reason="Forbidden", body=b"You have exceeded a secondary rate limit"
        )
        self.assertEqual(rate_limit_delay(secondary, now=0), 60)
        # The unauthenticated limit's status line alone is enough.
        self.assertEqual(rate_limit_delay(limited(), now=0), 60)
        for error in (
            limited(reason="Forbidden", body=b"Resource not accessible"),
            limited(404, reason="Not Found", x_ratelimit_remaining="0"),
        ):
            with self.subTest(code=error.code):
                self.assertIsNone(rate_limit_delay(error, now=0))

    def test_client_waits_for_quota_within_its_budget(self):
        github = GitHub("owner/repo", rate_limit_wait=120)
        slept = []
        github.sleep = slept.append
        with mock.patch(
            "tools.comparison.github.api_request",
            side_effect=[limited(retry_after="90"), {"sha": TARGET}],
        ) as request, mock.patch("sys.stderr", io.StringIO()):
            self.assertEqual(github.get("/commits/main"), {"sha": TARGET})
        self.assertEqual(slept, [90])
        self.assertEqual(request.call_count, 2)
        # The budget is shared by every later request of this client.
        with mock.patch(
            "tools.comparison.github.api_request",
            side_effect=[limited(retry_after="60")],
        ):
            with self.assertRaisesRegex(RateLimited, "retry after"):
                github.get("/commits/main")
        self.assertEqual(slept, [90])

    def test_quota_is_read_without_spending_it(self):
        github = GitHub("owner/repo")
        with mock.patch(
            "tools.comparison.github.api_request",
            return_value={"resources": {"core": {"remaining": 17}}},
        ) as request:
            self.assertEqual(github.core_remaining(), 17)
        self.assertEqual(request.call_args.args[0], "https://api.github.com/rate_limit")
        with mock.patch(
            "tools.comparison.github.api_request",
            side_effect=urllib.error.URLError("offline"),
        ):
            self.assertIsNone(github.core_remaining())

    def test_unnamed_secondary_limits_back_off_exponentially(self):
        self.assertEqual(rate_limit_delay(self.limited(), now=0, unnamed=2), 240)
        github = GitHub("owner/repo", rate_limit_wait=1000)
        slept = []
        github.sleep = slept.append
        def unnamed():
            return self.limited(reason="Forbidden", body=b"secondary rate limit")

        responses = [unnamed(), unnamed(), unnamed(),
                     self.limited(retry_after="30"), unnamed(), {"ok": True}]
        with mock.patch(
            "tools.comparison.github.api_request", side_effect=responses
        ), mock.patch("sys.stderr", io.StringIO()):
            self.assertEqual(github.get("/pulls/1"), {"ok": True})
        # A wait GitHub names resets the backoff.
        self.assertEqual(slept, [60, 120, 240, 30, 60])

    def test_other_errors_are_not_retried(self):
        github = GitHub("owner/repo", rate_limit_wait=3600)
        github.sleep = mock.Mock()
        with mock.patch(
            "tools.comparison.github.api_request",
            side_effect=[self.limited(reason="Forbidden", body=b"no access")],
        ):
            with self.assertRaises(urllib.error.HTTPError):
                github.get("/pulls/1")
        github.sleep.assert_not_called()


class GitRefDiscoveryTest(unittest.TestCase):
    """Anonymous discovery against a local origin carrying GitHub's PR refs."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        self.origin = root / "origin"
        self.origin.mkdir()
        git(self.origin, "init", "-q", "-b", "main")
        self.target = commit_files(self.origin, {"a.rs": "fn a() {}\n"}, "target")
        git(self.origin, "checkout", "-q", "-b", "feature")
        self.older = commit_files(self.origin, {"b.rs": "fn b() {}\n"}, "older")
        self.head = commit_files(self.origin, {"b.rs": "fn b(_: u8) {}\n"}, "head")
        git(self.origin, "checkout", "-q", "main")
        self.later = commit_files(self.origin, {"c.rs": "fn c() {}\n"}, "later")
        self.checkout = root / "checkout"
        git(root, "clone", "-q", self.origin.as_uri(), str(self.checkout))
        self.github = FakeGitHub()
        self.identity = new_identity("owner/repo", "pipeline", self.head)

    def publish_pull(self, head, base, number=7):
        """GitHub's refs/pull/N/head and its test merge of head into base."""
        self.merges = getattr(self, "merges", 0) + 1
        tree = git(self.origin, "rev-parse", head + "^{tree}")
        # Each recomputation is a new commit, as GitHub's are.
        message = "merge %d" % self.merges
        merge = git(
            self.origin, "commit-tree", tree, "-p", base, "-p", head, "-m", message
        )
        git(self.origin, "update-ref", "refs/pull/%d/head" % number, head)
        git(self.origin, "update-ref", "refs/pull/%d/merge" % number, merge)
        return merge

    def discover(self, **overrides):
        values = dict(pr_number=7, target_branch="main", attempts=2, sleep=mock.Mock())
        values.update(overrides)
        with mock.patch("sys.stderr", io.StringIO()):
            return discover_anonymous(
                self.github,
                self.identity,
                self.checkout,
                "feature",
                "main",
                values.pop("pr_number"),
                values.pop("target_branch"),
                **values,
            )

    def test_target_is_the_recorded_base_without_rest_reads(self):
        # GitHub keeps the recorded base until the PR is synchronized, even as
        # the target branch moves on; consumers compare against that base.
        merge = self.publish_pull(self.head, self.target)
        identity, found = self.discover()
        self.assertEqual(found, merge)
        self.assertEqual(
            {k: identity[k] for k in ("head_sha", "target_sha", "target_branch", "pr_number")},
            {"head_sha": self.head, "target_sha": self.target, "target_branch": "main",
             "pr_number": 7},
        )
        self.assertEqual(identity["pipeline_id"], "pipeline")
        self.assertEqual(self.github.calls, [])

    def test_pull_refs_that_lag_the_branch_are_awaited(self):
        self.publish_pull(self.older, self.target)

        def publish(_):
            self.publish_pull(self.head, self.target)

        identity, _ = pull_from_git(
            self.checkout, self.head, "feature", 7, "main", attempts=3, sleep=publish
        )
        self.assertEqual(identity["target_sha"], self.target)

    def test_merge_waits_end_at_the_discovery_deadline(self):
        git(self.origin, "update-ref", "refs/pull/7/head", self.head)
        now = [0.0]
        slept = []

        def sleep(seconds):
            slept.append(seconds)
            now[0] += seconds + 20  # each attempt's Git reads take time too

        with self.assertRaises(Unresolved):
            pull_from_git(
                self.checkout, self.head, "feature", 7, "main", attempts=12,
                delay=5, sleep=sleep, deadline_seconds=90, clock=lambda: now[0],
            )
        # Attempts run at 0, 25, 50, 75 and 100 seconds; another would start
        # after the 90-second deadline.
        self.assertEqual(slept, [5, 5, 5, 5])
        # A stalled merge fetch ends discovery's Git attempt at once.
        self.publish_pull(self.head, self.target)
        stalled = subprocess.TimeoutExpired(["git", "fetch"], 60)
        sleep = mock.Mock()
        with mock.patch(
            "tools.comparison.discover.merge_parents", side_effect=stalled
        ):
            with self.assertRaisesRegex(Unresolved, "timed out"):
                pull_from_git(
                    self.checkout, self.head, "feature", 7, "main", sleep=sleep
                )
        sleep.assert_not_called()

    def test_a_moved_branch_skips_like_rest_discovery(self):
        self.publish_pull(self.head, self.target)
        git(self.origin, "update-ref", "refs/heads/feature", self.later)
        self.assertEqual(self.discover(), (None, None))
        self.assertEqual(self.github.calls, [])

    def test_missing_or_stale_merges_fall_back_to_one_rest_read(self):
        self.github.branches["main"] = self.target
        self.github.open_pull(7, "feature", self.head)
        git(self.origin, "update-ref", "refs/pull/7/head", self.head)
        for stale in (False, True):
            with self.subTest(stale=stale):
                if stale:
                    self.publish_pull(self.older, self.target)
                    git(self.origin, "update-ref", "refs/pull/7/head", self.head)
                self.github.calls.clear()
                identity, merge = self.discover()
                self.assertIsNone(merge)
                self.assertEqual(identity["target_sha"], self.target)
                self.assertEqual(
                    self.github.calls, [("GET", "/repos/owner/repo/pulls/7")]
                )
        with self.assertRaises(Unresolved):
            pull_from_git(
                self.checkout, self.head, "feature", 7, "main", attempts=1
            )

    def test_events_without_a_pull_request_use_rest_discovery(self):
        self.github.branches["main"] = self.target
        self.github.open_pull(7, "feature", self.head)
        identity, merge = self.discover(pr_number=None, target_branch=None)
        self.assertEqual((identity["pr_number"], merge), (7, None))
        self.assertEqual(len(self.github.calls), 2)

    def test_default_branch_pushes_read_only_git(self):
        identity = new_identity("owner/repo", "pipeline", self.later)
        with mock.patch("sys.stderr", io.StringIO()):
            resolved, merge = discover_anonymous(
                self.github, identity, self.checkout, "main", "main"
            )
        self.assertEqual((resolved["target_sha"], merge), (self.later, None))
        self.assertTrue(current_from_git(self.checkout, resolved))
        stale = new_identity("owner/repo", "pipeline", self.target)
        with self.assertRaisesRegex(ValueError, "superseded"):
            discover_anonymous(self.github, stale, self.checkout, "main", "main")
        self.assertEqual(self.github.calls, [])

    def test_freshness_follows_the_pull_head_and_recorded_base(self):
        merge = self.publish_pull(self.head, self.target)
        identity, found = self.discover()
        self.assertEqual(found, merge)
        self.assertTrue(current_from_git(self.checkout, identity, found))
        # Merge parents name no target branch, so any recomputation (here
        # onto the same base, as when retargeting to an equal branch), a merge
        # GitHub dropped as it does on close, or no merge at all, leaves the
        # PR's state and target to the REST API.
        for change in (
            lambda: self.publish_pull(self.head, self.target),
            lambda: self.publish_pull(self.head, self.later),
            lambda: git(self.origin, "update-ref", "-d", "refs/pull/7/merge"),
        ):
            change()
            self.assertIsNone(current_from_git(self.checkout, identity, found))
        self.assertIsNone(current_from_git(self.checkout, identity, None))
        git(self.origin, "update-ref", "refs/pull/7/head", self.later)
        self.assertFalse(current_from_git(self.checkout, identity, found))
        self.assertFalse(current_from_git(self.checkout, identity, None))

    def test_unreachable_git_endpoints_defer_to_rest(self):
        self.github.branches["main"] = self.target
        self.github.open_pull(7, "feature", self.head)
        merge = self.publish_pull(self.head, self.target)
        identity, _ = self.discover()
        stalled = subprocess.TimeoutExpired(["git", "ls-remote"], 60)
        with mock.patch(
            "tools.comparison.discover._git_output", side_effect=stalled
        ):
            self.assertIsNone(current_from_git(self.checkout, identity, merge))
            self.github.calls.clear()
            resolved, found = self.discover()
        self.assertEqual((resolved["target_sha"], found), (self.target, None))
        self.assertEqual(self.github.calls, [("GET", "/repos/owner/repo/pulls/7")])

    def test_branch_names_cannot_become_options_or_patterns(self):
        self.assertEqual(branch_ref("feat/x-1"), "refs/heads/feat/x-1")
        for name in ("a b", "a..b", "x*", "", "a\nb"):
            with self.subTest(name=name):
                with self.assertRaises(ValueError):
                    branch_ref(name)


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
