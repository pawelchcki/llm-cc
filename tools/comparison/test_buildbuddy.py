"""Transport tests exercise scheduling without cloud credentials or GPUs.

Plans, workers and reports come from the deterministic llm-cc test build, so
every object crossing the store is the real schema version 2 artifact.
"""

import hashlib
import json
import os
import shlex
import signal
import subprocess
import sys
import zipfile
import argparse
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch

from . import buildbuddy
from .buildbuddy import (
    blob_id,
    bundle_command,
    prepare_plan,
    remote_worker,
    run_prepared,
    upload_plan,
    worker_request,
)
from .common import aggregate_report, llm_cc, pipeline_prefix, read_json, write_json
from .fixtures import commit_files, fake_scoring, git
from .github import GitHub
from .store import FilesystemStore

# Stands in for a bare host that passes llm-cc's execution-host verification:
# it drops --execution-host and runs the deterministic llm-cc.
VERIFIED_HOST = """#!{python}
import os, sys
arguments = sys.argv[1:]
if "--execution-host" in arguments:
    index = arguments.index("--execution-host")
    del arguments[index : index + 2]
os.execv({llm_cc!r}, [{llm_cc!r}] + arguments)
"""


class FakeAPI:
    def __init__(self, on_submit=None, complete=True):
        self.submissions = []
        self.cancelled = []
        self.on_submit = on_submit
        self.finished = complete

    def submit(self, request):
        self.submissions.append(request)
        if self.on_submit:
            self.on_submit(request)
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
        self.repo = self.root / "repo"
        self.repo.mkdir()
        git(self.repo, "init", "-q")
        target = commit_files(
            self.repo,
            {"main.c": "int main(void) { return 0; }\n", "README.md": "docs\n"},
            "base",
        )
        self.head = commit_files(
            self.repo,
            {"main.c": "int main(void) { return 1; }\n", "util.py": "x = 1\n"},
            "head",
        )
        self.identity = {
            "repository": "owner/repo",
            "pipeline_id": "parent-uuid",
            "head_sha": self.head,
            "target_sha": target,
            "base_sha": None,
            "target_branch": "release",
            "pr_number": 123,
            "started_at": "2026-09-11T00:00:00Z",
        }
        self.model, self.scoring = fake_scoring(self.root / "scoring")
        self.host = self.root / "execution-host.json"
        write_json(
            self.host,
            {
                "gpu_vendor": "amd",
                "gpu_pci_address": "0000:03:00.0",
                "gpu_arch": "gfx1100",
                "gpu_vram_bytes_min": 1,
                "resource_id": "bazzite-radeon-0",
                "runtime_files": {"/opt/rocm/lib/libamdhip64.so": "0" * 64},
            },
        )
        self.verified = self.root / "verified-llm-cc"
        self.verified.write_text(
            VERIFIED_HOST.format(
                python=sys.executable, llm_cc=os.path.abspath(llm_cc())
            )
        )
        self.verified.chmod(0o755)
        self.config = {
            "llm_cc": str(self.verified),
            "scoring_args": str(self.scoring),
            "cache": str(self.root / "store"),
            "execution_image": "none",
            "execution_host": str(self.host),
            "execution_commit": "2" * 40,
            "execution_repository": "https://github.com/owner/tool",
            "pool": "linux-amd64-rocm",
            "model": str(self.model),
            "max_workers": 1,
        }
        self.output = self.root / "out"

    def prepare(self):
        return prepare_plan(
            self.config, self.repo, self.head, self.identity, self.output
        )

    def score_everything(self):
        """Seed the store as an earlier comparison would have."""
        plan = self.prepare()
        for worker in plan["workers"]:
            subprocess.run(
                [
                    llm_cc(),
                    "compare",
                    "worker",
                    "--plan",
                    str(self.output / "plan.json"),
                    "--worker-id",
                    str(worker["worker_id"]),
                    "--output-dir",
                    str(self.root / "seed"),
                    "--cache",
                    self.config["cache"],
                    "--model",
                    str(self.model),
                ],
                check=True,
                capture_output=True,
            )

    def run_plan(self, api, config=None, **kwargs):
        return run_prepared(
            self.output / "plan.json",
            self.config if config is None else config,
            self.store,
            api,
            self.output,
            **kwargs,
        )

    def run_remote(self, request):
        """Run a submitted worker request the way the bare host would."""
        command = shlex.split(request["steps"][0]["run"])
        index = command.index("remote-worker")
        return buildbuddy.main(command[index:])

    def test_prepare_plans_misses_with_the_pinned_llm_cc(self):
        plan = self.prepare()
        self.assertEqual(plan["schema_version"], 2)
        self.assertEqual(plan["identity"]["base_sha"], self.identity["target_sha"])
        self.assertEqual(read_json(self.output / "identity.json")["pr_number"], 123)
        self.assertEqual(len(plan["workers"]), 1)
        for key in plan["workers"][0]["keys"]:
            object_id = plan["items"][key]["blob_id"]
            content = (self.output / "blobs" / object_id).read_bytes()
            self.assertEqual(blob_id(content, object_id), object_id)
        self.config["scoring_args"] = str(self.root / "auto.args")
        Path(self.config["scoring_args"]).write_text(
            "--model\n%s\n--backend\nauto\n" % self.model
        )
        with self.assertRaisesRegex(RuntimeError, "compare prepare failed"):
            self.prepare()
        self.config["cache_concurrency"] = 0
        with self.assertRaisesRegex(ValueError, "cache_concurrency"):
            self.prepare()

    def test_fully_cached_uses_no_gpu_or_worker_configuration(self):
        self.score_everything()
        plan = self.prepare()
        self.assertEqual(plan["workers"], [])
        api = FakeAPI()
        # The warm path needs no GPU host, pool, bundle or credentials.
        report = self.run_plan(api, config={})
        self.assertEqual(report["status"], "complete", report["errors"])
        self.assertEqual(api.submissions, [])
        self.assertEqual(api.cancelled, [])
        self.assertIsNotNone(
            self.store.get(pipeline_prefix(self.identity) + "report.json")
        )

    def test_miss_runs_a_bare_host_worker_and_collects_its_artifact(self):
        plan = self.prepare()
        statuses = []
        api = FakeAPI(
            on_submit=lambda request: statuses.append(self.run_remote(request))
        )
        report = self.run_plan(api)
        self.assertEqual(statuses, [0])
        self.assertEqual(report["status"], "complete", report["errors"])
        self.assertEqual(len(api.submissions), 1)
        request = api.submissions[0]
        self.assertEqual(request["commit_sha"], "2" * 40)
        self.assertEqual(request["timeout"], "2h")
        properties = request["platform_properties"]
        self.assertEqual(properties["Pool"], "linux-amd64-rocm")
        self.assertEqual(properties["container-image"], "none")
        self.assertEqual(properties["workload-isolation-type"], "none")
        self.assertEqual(
            properties["debug-executor-labels"], "gpu-resource=bazzite-radeon-0"
        )
        run = request["steps"][0]["run"]
        self.assertIn("--plan-sha256", run)
        self.assertIn("--execution-host " + str(self.host), run)
        self.assertIn(pipeline_prefix(self.identity), run)
        prefix = pipeline_prefix(self.identity)
        artifact = json.loads(self.store.get(prefix + "workers/0/worker-0.json"))
        self.assertEqual(artifact["schema_version"], 2)
        self.assertEqual(set(artifact["results"]), set(plan["workers"][0]["keys"]))
        # Each result is also in the shared cache, so the next plan is warm.
        self.assertEqual(self.prepare()["workers"], [])

    def test_remote_worker_refuses_a_changed_plan_and_still_reports(self):
        self.prepare()
        prefix, digest = upload_plan(self.store, self.output / "plan.json")
        args = argparse.Namespace(
            cache=self.config["cache"],
            store_options_json="{}",
            prefix=prefix,
            plan_sha256="0" * 64,
            worker_id=0,
            llm_cc=str(self.verified),
            model=str(self.model),
            execution_host=str(self.host),
        )
        self.assertEqual(remote_worker(args), 1)
        artifact = json.loads(self.store.get(prefix + "workers/0/worker-0.json"))
        self.assertEqual(artifact["schema_version"], 2)
        self.assertEqual(artifact["status"], "failed")
        self.assertIn("digest mismatch", artifact["errors"][0])
        # A blob that no longer matches its object ID is never scored.
        plan = read_json(self.output / "plan.json")
        object_id = plan["items"][plan["workers"][0]["keys"][0]]["blob_id"]
        self.store.put(prefix + "blobs/" + object_id, b"tampered")
        args.plan_sha256 = digest
        self.assertEqual(remote_worker(args), 1)
        artifact = json.loads(self.store.get(prefix + "workers/0/worker-0.json"))
        self.assertIn("corrupt source blob", artifact["errors"][0])

    def test_upload_verifies_blobs_against_their_object_ids(self):
        plan = self.prepare()
        object_id = plan["items"][plan["workers"][0]["keys"][0]]["blob_id"]
        (self.output / "blobs" / object_id).write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "changed before upload"):
            upload_plan(self.store, self.output / "plan.json")
        hashed = subprocess.run(
            ["git", "hash-object", "--stdin"],
            input=b"x = 1\n",
            capture_output=True,
            check=True,
        )
        self.assertEqual(blob_id(b"x = 1\n", "0" * 40), hashed.stdout.decode().strip())

    def test_missing_artifact_is_explicit_failure(self):
        self.prepare()
        report = self.run_plan(FakeAPI(complete=False))
        self.assertEqual(report["status"], "failed")
        self.assertTrue(any("without an artifact" in x for x in report["errors"]))
        self.assertTrue((self.output / "publication.json").is_file())

    def test_superseded_workers_are_cancelled_and_failure_report_retained(self):
        self.prepare()
        api = FakeAPI(complete=None)
        state = iter([True, False])
        report = self.run_plan(api, current=lambda: next(state))
        self.assertEqual(report["status"], "failed")
        self.assertEqual(len(api.cancelled), 1)

    def test_timeout_cancels_workers(self):
        self.prepare()
        api = FakeAPI(complete=None)
        report = self.run_plan(api, timeout_seconds=0)
        self.assertEqual(report["status"], "failed")
        self.assertEqual(len(api.cancelled), 1)

    def test_long_worker_freshness_checks_fit_anonymous_quota(self):
        self.prepare()
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
        self.prepare()
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
        self.prepare()
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
        self.score_everything()
        self.prepare()
        state = iter([True, False])
        report = self.run_plan(
            FakeAPI(), current=lambda: next(state), current_check_seconds=180
        )
        self.assertEqual(report["status"], "failed")
        self.assertIn(
            "comparison superseded before report publication", report["errors"]
        )

    def test_container_or_mutable_execution_is_rejected_before_submit(self):
        plan = self.prepare()
        for field, value in (
            # The scorer image has no Python to run the remote worker.
            ("execution_image", "registry.example/scorer@sha256:" + "1" * 64),
            ("execution_host", None),
            ("execution_host", "relative/execution-host.json"),
            ("execution_commit", "main"),
            ("pool", ""),
        ):
            with self.subTest(field=field, value=value):
                config = dict(self.config, **{field: value})
                with self.assertRaises(ValueError):
                    worker_request(config, plan, plan["workers"][0], "p/", "0" * 64)
        config = dict(self.config, store_options={"aws_secret_access_key": "x"})
        with self.assertRaisesRegex(ValueError, "worker_secret_env"):
            worker_request(config, plan, plan["workers"][0], "p/", "0" * 64)

    def test_bare_host_request_forces_host_isolation_and_verified_bundle(self):
        self.config.update(
            execution_bundle="/var/lib/llm-cc/packages/tool.zip",
            execution_bundle_sha256="f" * 64,
        )
        self.config["platform_properties"] = {"workload-isolation-type": "docker"}
        plan = self.prepare()
        request = worker_request(
            self.config, plan, plan["workers"][0], "prefix/", "0" * 64
        )
        self.assertTrue(request["skip_auto_checkout"])
        self.assertEqual(
            request["platform_properties"]["workload-isolation-type"], "none"
        )
        command = request["steps"][0]["run"]
        self.assertIn("python3 -I -c", command)
        self.assertIn("checksum mismatch", command)
        self.assertIn("f" * 64, command)
        self.config["execution_bundle"] = "relative.zip"
        with self.assertRaises(ValueError):
            worker_request(self.config, plan, plan["workers"][0], "prefix/", "0" * 64)

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

    def test_run_prepared_uploads_baseline_artifacts(self):
        self.score_everything()
        self.prepare()
        report = self.run_plan(FakeAPI(), config={})
        self.assertEqual(report["status"], "complete", report["errors"])
        prefix = pipeline_prefix(self.identity)
        for name in (
            "report.json",
            "report.md",
            "comment.md",
            "publication.json",
            "baseline.md",
            "baseline.json",
        ):
            with self.subTest(name=name):
                self.assertIsNotNone(self.store.get(prefix + name))
                self.assertTrue((self.output / name).is_file())
        baseline = json.loads(self.store.get(prefix + "baseline.json"))
        self.assertEqual(baseline["identity"], report["identity"])
        self.assertEqual(baseline["rankings"], report["rankings"]["head"])
        self.assertIn(
            "Baseline ranking for owner/repo@",
            self.store.get(prefix + "baseline.md").decode("utf-8"),
        )

    def test_pipeline_identity_cannot_escape_store(self):
        identity = dict(self.identity, pipeline_id="../../etc/passwd")
        self.assertRegex(pipeline_prefix(identity), r"^pipelines/[0-9a-f]{64}/$")

    def test_discovery_uses_actual_target_and_rejects_retargeting(self):
        github = GitHub("owner/repo")
        pull = {
            "number": 123,
            "state": "open",
            "head": {
                "sha": "a" * 40,
                "ref": "feature",
                "repo": {"full_name": "owner/repo"},
            },
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
        aggregate_report(self.root, identity=self.identity, errors=["analysis failed"])
        publication = json.loads((self.root / "publication.json").read_text())
        body = (self.root / "comment.md").read_bytes()
        self.assertEqual(
            publication["comment"]["sha256"], hashlib.sha256(body).hexdigest()
        )
        self.assertEqual(publication["comment"]["bytes"], len(body))

    @unittest.skipUnless(hasattr(signal, "setitimer"), "POSIX worker deadline")
    def test_remote_transport_deadline_interrupts_a_slow_store(self):
        uploaded = {}

        class SlowStore:
            def get(self, key):
                time.sleep(0.25)
                return None

            def put(self, key, value):
                uploaded[key] = value

        args = argparse.Namespace(
            cache="unused",
            store_options_json="{}",
            prefix="pipelines/" + "a" * 64 + "/",
            plan_sha256="0" * 64,
            worker_id=0,
            llm_cc="/bin/false",
            model="/missing",
            execution_host="/missing.json",
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
        artifact = json.loads(uploaded[args.prefix + "workers/0/worker-0.json"])
        self.assertEqual(artifact["status"], "failed")
        self.assertEqual(artifact["schema_version"], 2)


if __name__ == "__main__":
    unittest.main()
