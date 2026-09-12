"""Transport tests exercise scheduling without cloud credentials or GPUs."""

import hashlib
import json
import signal
import subprocess
import zipfile
import argparse
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch

from .buildbuddy import (
    GitHub,
    bundle_command,
    _pipeline_prefix,
    remote_worker,
    run_prepared,
    worker_request,
)
from .cache import FilesystemStore
from .common import CONTAINER_ENVIRONMENT_POLICY, digest, write_json
from .pipeline import failure_report


class FakeAPI:
    def __init__(self, on_submit=None, complete=True):
        self.submissions = []
        self.cancelled = []
        self.on_submit = on_submit
        self.finished = complete

    def submit(self, request):
        self.submissions.append(request)
        if self.on_submit:
            self.on_submit()
        return "00000000-0000-0000-0000-000000000001"

    def complete(self, invocation):
        return self.finished

    def cancel(self, invocation):
        self.cancelled.append(invocation)


class BuildBuddyTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.store = FilesystemStore(self.root / "store")
        self.identity = {
            "repository": "owner/repo",
            "pipeline_id": "parent-uuid",
            "head_sha": "a" * 40,
            "target_sha": "b" * 40,
            "base_sha": "c" * 40,
            "target_branch": "release",
            "pr_number": 123,
            "started_at": "2026-09-11T00:00:00Z",
        }
        self.image = "registry.example/scorer@sha256:" + "1" * 64
        profile = {
            "scoring": {"tau": 0.67},
            "build": {
                "execution_image": self.image,
                "container_environment_policy": CONTAINER_ENVIRONMENT_POLICY,
            },
        }
        self.plan = {
            "schema_version": 1,
            "identity": self.identity,
            "fingerprint": digest(profile),
            "profile": profile,
            "inventories": {"base": [], "head": []},
            "changes": [],
            "items": {},
            "hits": {},
            "workers": [],
            "cache_stats": {"items": 0, "hits": 0, "misses": 0},
        }
        self.config = {
            "cache": str(self.root / "store"),
            "execution_image": self.image,
            "execution_commit": "2" * 40,
            "execution_repository": "https://github.com/owner/tool",
            "pool": "a10",
            "scorer": "/opt/llm-cc/bin/llm-cc",
            "model": "/models/model.gguf",
            "installed_root": "/opt/llm-cc",
        }
        self.plan_path = self.root / "plan.json"

    def miss(self):
        data = b"int x;\n"
        sha = hashlib.sha256(data).hexdigest()
        key = digest([sha, "c", self.plan["fingerprint"]])
        item = {
            "key": key,
            "content_sha256": sha,
            "language": "c",
            "size": len(data),
            "blob": "blobs/" + sha,
        }
        self.plan["items"][key] = item
        self.plan["workers"] = [{"worker_id": 0, "keys": [key], "bytes": len(data)}]
        self.plan["cache_stats"].update(items=1, misses=1)
        self.plan["inventories"]["head"] = [
            dict(item, path="x.c", category="runtime", reason=None, scorable=True)
        ]
        (self.root / "blobs").mkdir()
        (self.root / item["blob"]).write_bytes(data)
        return item

    def run_plan(self, api, **kwargs):
        write_json(self.plan_path, self.plan)
        return run_prepared(
            self.plan_path, self.config, self.store, api, self.root / "out", **kwargs
        )

    def test_fully_cached_uses_no_gpu_or_worker_configuration(self):
        api = FakeAPI()
        self.config = {}  # warm path does not need a GPU image, pool, or credentials
        report = self.run_plan(api)
        self.assertEqual(report["status"], "complete")
        self.assertEqual(api.submissions, [])
        self.assertEqual(api.cancelled, [])
        self.assertIsNotNone(
            self.store.get(_pipeline_prefix(self.identity) + "report.json")
        )

    def test_miss_submits_immutable_worker_and_collects_parent_artifact(self):
        item = self.miss()
        result = dict(
            item,
            schema_version=1,
            fingerprint=self.plan["fingerprint"],
            llm_cc=4.0,
            token_count=2,
        )
        result.pop("blob")
        result.pop("size")
        artifact = {
            "schema_version": 1,
            "identity": self.identity,
            "fingerprint": self.plan["fingerprint"],
            "worker_id": 0,
            "status": "complete",
            "errors": [],
            "results": {item["key"]: result},
        }

        def publish():
            self.store.put(
                _pipeline_prefix(self.identity) + "workers/0/worker-0.json",
                json.dumps(artifact).encode(),
            )

        api = FakeAPI(on_submit=publish)
        report = self.run_plan(api)
        self.assertEqual(report["status"], "complete")
        self.assertEqual(len(api.submissions), 1)
        request = api.submissions[0]
        self.assertEqual(request["commit_sha"], "2" * 40)
        self.assertEqual(request["timeout"], "2h")
        self.assertEqual(request["platform_properties"]["Pool"], "a10")
        self.assertEqual(request["platform_properties"]["container-image"], self.image)
        self.assertIn("--plan-sha256", request["steps"][0]["run"])
        self.assertIn(_pipeline_prefix(self.identity), request["steps"][0]["run"])

    def test_missing_artifact_is_explicit_failure(self):
        self.miss()
        report = self.run_plan(FakeAPI(complete=False))
        self.assertEqual(report["status"], "failed")
        self.assertTrue(any("without an artifact" in x for x in report["errors"]))
        self.assertTrue((self.root / "out/publication.json").is_file())

    def test_superseded_workers_are_cancelled_and_failure_report_retained(self):
        self.miss()
        api = FakeAPI(complete=None)
        state = iter([True, False])
        report = self.run_plan(api, current=lambda: next(state))
        self.assertEqual(report["status"], "failed")
        self.assertEqual(len(api.cancelled), 1)

    def test_timeout_cancels_workers(self):
        self.miss()
        api = FakeAPI(complete=None)
        report = self.run_plan(api, timeout_seconds=0)
        self.assertEqual(report["status"], "failed")
        self.assertEqual(len(api.cancelled), 1)

    def test_long_worker_freshness_checks_fit_anonymous_quota(self):
        self.miss()
        now = [0]
        checks = []
        polls = []
        api = FakeAPI()

        def complete(_):
            polls.append(now[0])
            return True if now[0] >= 6600 else None

        def current():
            checks.append(now[0])
            return True

        def sleep(seconds):
            now[0] += seconds

        api.complete = complete
        with (
            patch(
                "tools.comparison.buildbuddy.time.monotonic",
                side_effect=lambda: now[0],
            ),
            patch("tools.comparison.buildbuddy.time.sleep", side_effect=sleep),
            patch("builtins.print"),
        ):
            self.run_plan(api, current=current, current_check_seconds=180)
        self.assertEqual(len(polls), 661)
        self.assertEqual(checks[0], 0)
        self.assertEqual(checks[-1], 6600)
        self.assertEqual(len(checks), 38)
        self.assertLess(sum(value < 3600 for value in checks) + 2, 60)

    def test_periodic_freshness_check_cancels_superseded_worker(self):
        self.miss()
        now = [0]
        api = FakeAPI(complete=None)

        def sleep(seconds):
            now[0] += seconds

        with (
            patch(
                "tools.comparison.buildbuddy.time.monotonic",
                side_effect=lambda: now[0],
            ),
            patch("tools.comparison.buildbuddy.time.sleep", side_effect=sleep),
            patch("builtins.print"),
        ):
            report = self.run_plan(
                api, current=lambda: now[0] < 180, current_check_seconds=180
            )
        self.assertEqual(now[0], 180)
        self.assertEqual(report["status"], "failed")
        self.assertEqual(len(api.cancelled), 1)

    def test_final_freshness_check_rejects_change_between_periodic_checks(self):
        self.miss()
        api = FakeAPI()
        with patch("tools.comparison.buildbuddy.time.monotonic", return_value=0):
            state = iter([True, False])
            report = self.run_plan(
                api, current=lambda: next(state), current_check_seconds=180
            )
        self.assertEqual(report["status"], "failed")
        self.assertIn(
            "comparison superseded before report publication", report["errors"]
        )

    def test_final_freshness_check_runs_for_fully_cached_comparison(self):
        state = iter([True, False])
        report = self.run_plan(
            FakeAPI(), current=lambda: next(state), current_check_seconds=180
        )
        self.assertEqual(report["status"], "failed")
        self.assertIn(
            "comparison superseded before report publication", report["errors"]
        )

    def test_mutable_or_mismatched_execution_is_rejected_before_submit(self):
        self.miss()
        for field, value in (
            ("execution_image", "registry.example/scorer:latest"),
            ("execution_commit", "main"),
        ):
            with self.subTest(field=field):
                config = dict(self.config, **{field: value})
                with self.assertRaises(ValueError):
                    worker_request(
                        config, self.plan, self.plan["workers"][0], "prefix/", "0" * 64
                    )

    def test_bare_host_request_forces_host_isolation_and_verified_bundle(self):
        self.config.update(
            execution_image="none",
            execution_bundle="/var/lib/llm-cc/packages/tool.zip",
            execution_bundle_sha256="f" * 64,
        )
        self.config["platform_properties"] = {"workload-isolation-type": "docker"}
        self.plan["profile"]["build"].update(
            execution_image="none", execution_host={"resource_id": "bazzite-radeon-0"}
        )
        self.miss()
        request = worker_request(
            self.config, self.plan, self.plan["workers"][0], "prefix/", "0" * 64
        )
        self.assertTrue(request["skip_auto_checkout"])
        self.assertEqual(
            request["platform_properties"]["workload-isolation-type"], "none"
        )
        self.assertEqual(
            request["platform_properties"]["debug-executor-labels"],
            "gpu-resource=bazzite-radeon-0",
        )
        command = request["steps"][0]["run"]
        self.assertIn("python3 -I -c", command)
        self.assertIn("checksum mismatch", command)
        self.assertIn("f" * 64, command)
        self.plan["profile"]["build"].pop("execution_host")
        with self.assertRaises(ValueError):
            worker_request(
                self.config, self.plan, self.plan["workers"][0], "prefix/", "0" * 64
            )

    def test_bundle_executes_only_verified_bytes_with_expected_arguments(self):
        archive = self.root / "code.zip"
        with zipfile.ZipFile(archive, "w") as bundle:
            bundle.writestr(
                zipfile.ZipInfo("probe.py", date_time=(1980, 1, 1, 0, 0, 0)),
                "import sys; print(sys.argv[1:])",
            )
        checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
        command = bundle_command(
            ["python3", "-m", "probe", "--test", "one two"], str(archive), checksum
        )
        completed = subprocess.run(command, capture_output=True, text=True, check=True)
        self.assertEqual(completed.stdout.strip(), "['--test', 'one two']")
        archive.write_bytes(b"replaced")
        completed = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("execution bundle checksum mismatch", completed.stderr)

    def test_pipeline_identity_cannot_escape_store(self):
        identity = dict(self.identity, pipeline_id="../../etc/passwd")
        self.assertRegex(_pipeline_prefix(identity), r"^pipelines/[0-9a-f]{64}/$")

    def test_discovery_uses_actual_target_and_rejects_retargeting(self):
        github = GitHub("owner/repo")
        pull = {
            "number": 123,
            "state": "open",
            "head": {"sha": "a" * 40, "ref": "feature"},
            "base": {
                "sha": "b" * 40,
                "ref": "release",
                "repo": {"full_name": "owner/repo"},
            },
        }
        github.get = lambda path: [pull] if "/commits/" in path else pull
        identity = github.discover("a" * 40, "feature", "main")
        self.assertEqual(identity["target_branch"], "release")
        self.assertTrue(github.current(identity))
        pull["base"]["sha"] = "d" * 40
        self.assertFalse(github.current(identity))

    def test_no_current_pr_skips_and_ambiguous_pr_errors(self):
        github = GitHub("owner/repo")
        github.get = lambda _: []
        self.assertIsNone(github.discover("a" * 40, "feature", "main"))

    def test_failure_envelope_matches_bounded_comment(self):
        failure_report(self.root, self.identity, "0" * 64, ["analysis failed"])
        publication = json.loads((self.root / "publication.json").read_text())
        body = (self.root / "comment.md").read_bytes()
        self.assertEqual(
            publication["comment"]["sha256"], hashlib.sha256(body).hexdigest()
        )
        self.assertEqual(publication["comment"]["bytes"], len(body))

    @unittest.skipUnless(hasattr(signal, "setitimer"), "POSIX worker deadline")
    def test_remote_transport_deadline_interrupts_a_slow_store(self):
        class SlowStore:
            def get(self, key):
                time.sleep(0.25)
                return None

            def put(self, key, value):
                pass

        args = argparse.Namespace(
            cache="unused",
            store_options_json="{}",
            prefix="pipelines/" + "a" * 64 + "/",
            plan_sha256="0" * 64,
            worker_id=0,
            scorer="/bin/false",
            model="/missing",
            installed_root="/tmp",
        )
        began = time.monotonic()
        # Keep the test short without weakening the production 110-minute limit.
        with (
            patch("tools.comparison.buildbuddy.open_store", return_value=SlowStore()),
            patch(
                "tools.comparison.buildbuddy.Deadline",
                side_effect=lambda seconds: __import__(
                    "tools.comparison.deadline", fromlist=["Deadline"]
                ).Deadline(0.05 if seconds > 1 else seconds),
            ),
        ):
            self.assertEqual(remote_worker(args), 1)
        self.assertLess(time.monotonic() - began, 0.2)


if __name__ == "__main__":
    unittest.main()
