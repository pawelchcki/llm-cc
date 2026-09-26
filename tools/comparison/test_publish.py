"""Publisher ordering, suppression and validation against in-memory GitHub."""

import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import urllib.error

from .__main__ import main
from .cache import FilesystemStore
from .common import pipeline_prefix, write_json
from .fixtures import FakeGitHub
from .common import aggregate_report
from .publish import (
    COMMENT_MARKER,
    PublicationError,
    comment_body,
    load_publication,
    marker_key,
    publish,
    store_report,
)

REPOSITORY = "owner/repo"
TARGET = "b" * 40


def head(n):
    return "%040x" % n


class PublishFixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.store = FilesystemStore(self.root / "store")
        self.github = FakeGitHub(REPOSITORY)
        self.github.branches["main"] = TARGET
        self.pull = self.github.open_pull(7, "feature", head(1))

    def identity(self, pipeline_id, sha, pr_number=7, target=TARGET):
        return {
            "repository": REPOSITORY,
            "pipeline_id": pipeline_id,
            "head_sha": sha,
            "target_sha": target,
            "base_sha": target,
            "target_branch": "main",
            "pr_number": pr_number,
            "started_at": "2026-09-23T00:00:00Z",
        }

    def stored(self, pipeline_id, sha, message="analysis finished", **fields):
        identity = self.identity(pipeline_id, sha, **fields)
        directory = self.root / ("report-" + pipeline_id)
        aggregate_report(directory, identity=identity, errors=[message])
        store_report(self.store, directory)
        return identity

    def publish(self, pipeline_id, sha, ordinal, **options):
        options.setdefault("comment_author", self.github.login)
        return publish(
            self.store,
            self.github,
            repository=REPOSITORY,
            pipeline_id=pipeline_id,
            head=sha,
            ordinal=ordinal,
            **options,
        )

    def marker(self):
        return json.loads(self.store.get(marker_key(REPOSITORY, 7)))

    def comments(self):
        return self.github.pull_comments(7)


class PublishTest(PublishFixture):
    def test_first_publication_creates_one_marked_comment(self):
        self.stored("101", head(1))
        outcome, _ = self.publish("101", head(1), 1)
        self.assertEqual(outcome, "published")
        [comment] = self.comments()
        lines = comment["body"].split("\n")
        self.assertEqual(lines[0], COMMENT_MARKER)
        self.assertEqual(lines[1], "<!-- llm-cc-comparison pipeline=101 ordinal=1 -->")
        self.assertIn("analysis finished", comment["body"])
        self.assertEqual(
            self.marker(),
            {
                "schema_version": 1,
                "ordinal": 1,
                "pipeline_id": "101",
                "head_sha": head(1),
                "state": "published",
                "comment_id": comment["id"],
            },
        )

    def test_successive_pushes_update_the_same_comment(self):
        self.stored("101", head(1), "first push")
        self.publish("101", head(1), 1)
        self.pull["head"]["sha"] = head(2)
        self.stored("102", head(2), "second push")
        self.assertEqual(self.publish("102", head(2), 2)[0], "published")
        [comment] = self.comments()
        self.assertIn("second push", comment["body"])
        self.assertNotIn("first push", comment["body"])

    def test_stale_ordinal_is_rejected_without_touching_the_comment(self):
        self.stored("101", head(1), "older pipeline")
        self.pull["head"]["sha"] = head(2)
        self.stored("102", head(2), "newer pipeline")
        self.publish("102", head(2), 2)
        outcome, reason = self.publish("101", head(1), 1)
        self.assertEqual(outcome, "stale")
        self.assertIn("102", reason)
        [comment] = self.comments()
        self.assertIn("newer pipeline", comment["body"])
        self.assertEqual(self.marker()["ordinal"], 2)

    def test_lost_response_after_reservation_still_blocks_older_pipelines(self):
        self.pull["head"]["sha"] = head(2)
        self.stored("102", head(2), "newer pipeline")
        self.github.lose_next_write = True
        with self.assertRaises(urllib.error.URLError):
            self.publish("102", head(2), 2)
        # The comment exists, but its ID never reached the marker.
        self.assertEqual(self.marker()["state"], "reserved")
        self.assertIsNone(self.marker()["comment_id"])
        self.stored("101", head(1), "older pipeline")
        self.assertEqual(self.publish("101", head(1), 1)[0], "stale")
        # A retry of the reserving pipeline adopts its comment, never a duplicate.
        self.assertEqual(self.publish("102", head(2), 2)[0], "published")
        [comment] = self.comments()
        self.assertIn("newer pipeline", comment["body"])
        self.assertEqual(self.marker()["comment_id"], comment["id"])

    def test_closed_retargeted_and_superseded_prs_are_suppressed(self):
        for name, change in (
            ("closed", lambda pull: pull.update(state="closed")),
            ("retargeted", lambda pull: pull["base"].update(ref="release")),
            ("new target", lambda pull: pull["base"].update(sha="c" * 40)),
            ("new head", lambda pull: pull["head"].update(sha=head(9))),
        ):
            with self.subTest(name=name):
                self.setUp()
                self.stored("101", head(1))
                change(self.pull)
                outcome, _ = self.publish("101", head(1), 1)
                self.assertEqual(outcome, "suppressed")
                self.assertEqual(self.comments(), [])
                self.assertEqual(self.marker()["state"], "suppressed")
                # The suppressed reservation still outranks older pipelines.
                self.stored("100", head(1))
                self.assertEqual(self.publish("100", head(1), 0)[0], "stale")

    def test_other_authors_marker_comments_are_ignored(self):
        forged = self.github.add_comment(
            7,
            comment_body(b"forged\n", "999", 999),
            "mallory",
        )
        self.stored("101", head(1), "genuine")
        self.assertEqual(self.publish("101", head(1), 1)[0], "published")
        comments = self.comments()
        self.assertEqual(len(comments), 2)
        self.assertEqual(comments[0]["body"], forged["body"])
        self.assertEqual(comments[1]["user"]["login"], self.github.login)
        self.assertIn("genuine", comments[1]["body"])

    def test_own_comment_from_a_newer_pipeline_is_never_overwritten(self):
        # For example after the marker store was reset.
        self.github.add_comment(
            7, comment_body(b"newer\n", "205", 5), self.github.login
        )
        self.stored("103", head(1), "older")
        self.assertEqual(self.publish("103", head(1), 3)[0], "stale")
        [comment] = self.comments()
        self.assertIn("newer", comment["body"])

    def test_web_edited_comment_with_crlf_is_still_recognized(self):
        own = self.github.add_comment(
            7, comment_body(b"old\n", "100", 0).replace("\n", "\r\n"), self.github.login
        )
        self.stored("101", head(1), "updated")
        self.publish("101", head(1), 1)
        [comment] = self.comments()
        self.assertEqual(comment["id"], own["id"])
        self.assertIn("updated", comment["body"])

    def test_comment_author_matches_the_login_in_any_case(self):
        self.stored("101", head(1), "first push")
        self.publish("101", head(1), 1, comment_author=self.github.login.upper())
        self.pull["head"]["sha"] = head(2)
        self.stored("102", head(2), "second push")
        self.publish("102", head(2), 2, comment_author=self.github.login.upper())
        [comment] = self.comments()
        self.assertIn("second push", comment["body"])

    def test_interrupted_report_storage_is_never_published(self):
        # A complete earlier attempt under the same pipeline ID, then a retry
        # whose storage fails part-way: neither report may look current.
        store = self.store
        for failing in ("report.json", "baseline.json"):
            with self.subTest(failing=failing):
                identity = self.stored("101", head(1), "first attempt")
                directory = self.root / "report-101"
                aggregate_report(directory, identity=identity, errors=["retried attempt"])

                class Interrupted:
                    def put(self, key, value):
                        if key.endswith(failing):
                            raise OSError("store went away")
                        store.put(key, value)

                with self.assertRaisesRegex(OSError, "went away"):
                    store_report(Interrupted(), directory)
                prefix = pipeline_prefix(identity)
                self.assertIsNone(load_publication(self.store, prefix))

    def test_tampered_comment_is_rejected(self):
        self.stored("101", head(1))
        prefix = pipeline_prefix({"repository": REPOSITORY, "pipeline_id": "101"})
        self.store.put(prefix + "comment.md", b"## looks legitimate\n")
        with self.assertRaisesRegex(PublicationError, "comment.md"):
            self.publish("101", head(1), 1)
        self.assertEqual(self.comments(), [])

    def test_report_must_match_the_trusted_pipeline_inputs(self):
        self.stored("101", head(1))
        with self.assertRaisesRegex(PublicationError, "head_sha"):
            self.publish("101", head(2), 1)
        identity = self.identity("101", head(1), pr_number=8)
        with self.assertRaisesRegex(PublicationError, "pr_number"):
            self.publish("101", head(1), 1, identity=identity)
        with self.assertRaisesRegex(PublicationError, "pipeline ID"):
            self.publish("101 -->", head(1), 1)

    def test_failure_is_published_without_a_report(self):
        identity = self.identity("101", head(1))
        with self.assertRaisesRegex(PublicationError, "no stored report"):
            self.publish("101", head(1), 1)
        outcome, _ = self.publish("101", head(1), 1, identity=identity)
        self.assertEqual(outcome, "published")
        [comment] = self.comments()
        self.assertIn("Status: **failed**", comment["body"])
        self.assertIn(
            "no comparison report was stored for pipeline 101", comment["body"]
        )
        prefix = pipeline_prefix(identity)
        self.assertIsNotNone(self.store.get(prefix + "report.json"))
        # An explicit failure replaces even a stored report.
        self.publish(
            "101", head(1), 1, identity=identity, failure="GPU image pull failed"
        )
        self.assertIn("GPU image pull failed", self.comments()[0]["body"])

    def test_default_branch_run_only_stores_its_report(self):
        identity = self.stored("201", TARGET, pr_number=None)
        self.github.calls.clear()
        outcome, _ = self.publish("201", TARGET, 1, identity=identity)
        self.assertEqual(outcome, "stored")
        self.assertEqual(self.github.calls, [])
        self.assertIsNone(self.store.get(marker_key(REPOSITORY, None)))

    def test_malformed_marker_stops_publication(self):
        self.stored("101", head(1))
        self.store.put(marker_key(REPOSITORY, 7), b'{"ordinal": "high"}')
        with self.assertRaisesRegex(PublicationError, "malformed"):
            self.publish("101", head(1), 1)

    def test_concurrent_newer_reservation_wins_before_the_comment_update(self):
        # Without CI serialization a newer publisher can reserve mid-flight.
        self.stored("101", head(1), "older")
        original = self.github.issue_comments

        def interleaved(number):
            self.store.put(
                marker_key(REPOSITORY, 7),
                json.dumps(
                    {
                        "schema_version": 1,
                        "ordinal": 2,
                        "pipeline_id": "102",
                        "head_sha": head(2),
                        "state": "reserved",
                        "comment_id": None,
                    }
                ).encode(),
            )
            return original(number)

        self.github.issue_comments = interleaved
        self.assertEqual(self.publish("101", head(1), 1)[0], "stale")
        self.assertEqual(self.comments(), [])

    def test_author_defaults_to_the_token_login(self):
        self.stored("101", head(1))
        self.publish("101", head(1), 1, comment_author=None)
        self.assertEqual(self.comments()[0]["user"]["login"], self.github.login)

        def forbidden():
            raise urllib.error.HTTPError("/user", 403, "Forbidden", {}, None)

        self.github.authenticated_login = forbidden
        with self.assertRaisesRegex(PublicationError, "--comment-author"):
            self.publish("101", head(1), 1, comment_author=None)

    def test_store_report_refuses_reports_that_name_no_pipeline(self):
        directory = self.root / "anonymous"
        aggregate_report(directory, identity=None, errors=["unreadable plan"])
        with self.assertRaisesRegex(PublicationError, "repository and pipeline"):
            store_report(self.store, directory)
        stored = self.root / "mismatched"
        aggregate_report(stored, identity=self.identity("101", head(1)), errors=["x"])
        with self.assertRaisesRegex(PublicationError, "pipeline_id"):
            store_report(
                self.store, stored, {"repository": REPOSITORY, "pipeline_id": "999"}
            )


class PublishCommandTest(PublishFixture):
    def run_main(self, *arguments):
        output, errors = io.StringIO(), io.StringIO()
        with (
            mock.patch("tools.comparison.__main__.GitHub", return_value=self.github),
            contextlib.redirect_stdout(output),
            contextlib.redirect_stderr(errors),
        ):
            status = main(list(arguments))
        return status, output.getvalue(), errors.getvalue()

    def arguments(self, pipeline_id, sha, ordinal, *extra):
        return (
            "publish",
            "--cache",
            str(self.root / "store"),
            "--repository",
            REPOSITORY,
            "--pipeline-id",
            pipeline_id,
            "--head",
            sha,
            "--ordinal",
            str(ordinal),
            "--comment-author",
            self.github.login,
            *extra,
        )

    def test_advisory_outcomes_succeed_and_errors_fail(self):
        self.stored("102", head(1))
        status, output, _ = self.run_main(*self.arguments("102", head(1), 2))
        self.assertEqual((status, output.split(":")[1].strip()), (0, "published"))
        self.stored("101", head(1))
        status, output, _ = self.run_main(*self.arguments("101", head(1), 1))
        self.assertEqual((status, output.split(":")[1].strip()), (0, "stale"))
        status, _, errors = self.run_main(*self.arguments("103", head(1), 3))
        self.assertEqual(status, 1)
        self.assertIn("no stored report", errors)
        identity = self.root / "identity.json"
        write_json(identity, self.identity("103", head(1)))
        status, output, _ = self.run_main(
            *self.arguments("103", head(1), 3, "--identity", str(identity))
        )
        self.assertEqual((status, output.split(":")[1].strip()), (0, "published"))


if __name__ == "__main__":
    unittest.main()
