"""CPU coordinator and remote-worker transport for BuildBuddy.

The coordinator plans with `llm-cc compare prepare` and uploads each miss's
blob; bare-host GPU workers fetch them and score with `llm-cc compare worker`
under the host's execution contract. Run uses the public enterprise API;
cancellation uses CancelExecutions from BuildBuddyService. Credentials remain
environment inputs, never plan artifacts.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import string
import subprocess
import tempfile
import time
import urllib.parse

from .common import (
    aggregate_report,
    pipeline_prefix,
    read_json,
    write_json,
)
from .deadline import Deadline, DeadlineExceeded
from .discover import discover_identity, fetch_target, new_identity
from .github import GitHub, api_request
from .publish import store_report
from .store import open_store

# The remote worker's whole budget, including transport and upload.
WORKER_SECONDS = 6600
# No plan is larger; a larger object is corrupt and is never buffered whole.
PLAN_BYTES = 256 * 1024 * 1024


class BuildBuddy:
    def __init__(self, endpoint, api_key):
        if not endpoint.startswith("https://"):
            raise ValueError("BuildBuddy endpoint must use HTTPS")
        self.endpoint, self.api_key = endpoint.rstrip("/"), api_key

    def api(self, method, payload):
        return api_request(
            self.endpoint + "/api/v1/" + method,
            self.api_key,
            payload,
            "x-buildbuddy-api-key",
        )

    def submit(self, request):
        response = self.api("Run", request)
        invocation = response.get("invocation_id", response.get("invocationId"))
        if not isinstance(invocation, str) or not re.fullmatch(
            r"[0-9a-f-]{36}", invocation
        ):
            raise ValueError("Run returned no valid invocation ID")
        return invocation

    def complete(self, invocation):
        result = self.api("GetInvocation", {"selector": {"invocation_id": invocation}})
        records = result.get("invocation", [])
        if not records:
            return None
        record = records[0]
        state = record.get("invocationStatus", record.get("invocation_status"))
        if state in (1, "COMPLETE_INVOCATION_STATUS"):
            return record.get("success") is True
        if state in (3, "DISCONNECTED_INVOCATION_STATUS"):
            return False
        return None

    def cancel(self, invocation):
        # CancelExecutionsRequest invocation_id is protobuf field 2. UUIDs have
        # single-byte lengths; no protobuf runtime is needed in the CPU image.
        encoded = invocation.encode("ascii")
        if not re.fullmatch(rb"[0-9a-f-]{36}", encoded):
            raise ValueError("invalid cancellation invocation ID")
        return api_request(
            self.endpoint + "/rpc/BuildBuddyService/CancelExecutions",
            self.api_key,
            b"\x12" + bytes([len(encoded)]) + encoded,
            "x-buildbuddy-api-key",
            "application/proto",
        )


def blob_id(content, object_id):
    """The Git object ID of `content` in the format `object_id` uses."""
    if not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", object_id):
        raise ValueError("invalid blob ID")
    algorithm = "sha1" if len(object_id) == 40 else "sha256"
    return hashlib.new(algorithm, b"blob %d\0" % len(content) + content).hexdigest()


def upload_plan(store, plan_path):
    """Upload every assigned blob, then the plan's exact bytes and digest."""
    plan_path = Path(plan_path)
    payload = plan_path.read_bytes()
    plan = json.loads(payload)
    prefix = pipeline_prefix(plan["identity"])
    uploaded = set()
    for worker in plan["workers"]:
        for key in worker["keys"]:
            object_id = plan["items"][key]["blob_id"]
            if object_id in uploaded:
                continue
            content = (plan_path.parent / "blobs" / object_id).read_bytes()
            if blob_id(content, object_id) != object_id:
                raise ValueError("preparation blob changed before upload")
            store.put(prefix + "blobs/" + object_id, content)
            uploaded.add(object_id)
    store.put(prefix + "plan.json", payload)
    return prefix, hashlib.sha256(payload).hexdigest()


def bundle_command(command, bundle, bundle_sha):
    """Run a Python module from checksum-verified private archive bytes."""
    if (
        len(command) < 3
        or command[:2] != ["python3", "-m"]
        or not Path(bundle).is_absolute()
        or not re.fullmatch(r"[0-9a-f]{64}", bundle_sha)
    ):
        raise ValueError(
            "bundle requires Python module command, absolute path, and SHA-256"
        )
    # Isolated Python ignores inherited module paths/user site packages. Copy
    # verified bytes privately so replacement of the source cannot race imports.
    bootstrap = (
        "import hashlib,pathlib,runpy,sys,tempfile\n"
        "p,d,m=sys.argv[1:4]; del sys.argv[1:4]\n"
        "b=pathlib.Path(p).read_bytes()\n"
        "if hashlib.sha256(b).hexdigest()!=d: raise RuntimeError('execution bundle checksum mismatch')\n"
        "with tempfile.TemporaryDirectory(prefix='llm-cc-code-') as t:\n"
        " q=pathlib.Path(t)/'code.zip'; q.write_bytes(b)\n"
        " sys.path.insert(0,str(q))\n"
        " runpy.run_module(m,run_name='__main__',alter_sys=True)"
    )
    return ["python3", "-I", "-c", bootstrap, bundle, bundle_sha, command[2]] + command[
        3:
    ]


def cache_concurrency(config):
    """The validated bound on concurrent result-cache reads in preparation."""
    value = config.get("cache_concurrency", 8)
    if type(value) is not int or not 1 <= value <= 64:
        raise ValueError("cache_concurrency must be between 1 and 64")
    return value


def execution_host(config):
    """The bare-host contract a worker verifies: PCI GPU, lock and runtimes."""
    path = config.get("execution_host")
    if config.get("execution_image") != "none" or not path:
        # Only llm-cc runs in the scorer image, and it cannot fetch a plan
        # from BuildBuddy's side; workers need the host's Python bundle.
        raise ValueError(
            "BuildBuddy workers run on a bare host: set execution_image to "
            "none and execution_host to its execution-host.json"
        )
    if not Path(path).is_absolute():
        raise ValueError("execution_host must be an absolute path")
    host = read_json(path)
    if not isinstance(host, dict) or not re.fullmatch(
        r"[a-z0-9][a-z0-9_-]{0,63}", str(host.get("resource_id", ""))
    ):
        raise ValueError("execution host needs a valid resource_id")
    return host


def worker_request(config, plan, worker, prefix, plan_digest):
    host = execution_host(config)
    commit = config["execution_commit"]
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("execution_commit must pin the comparison implementation")
    if not config["pool"]:
        raise ValueError("GPU runner pool is required")
    options = config.get("store_options", {})
    if any("secret" in k or "access_key" in k or "token" in k for k in options):
        raise ValueError(
            "pass store credentials via worker_secret_env, not store_options"
        )
    command = [
        "python3",
        "-m",
        "tools.comparison.buildbuddy",
        "remote-worker",
        "--cache",
        config["cache"],
        "--prefix",
        prefix,
        "--plan-sha256",
        plan_digest,
        "--worker-id",
        str(worker["worker_id"]),
        "--llm-cc",
        config["llm_cc"],
        "--model",
        config["model"],
        "--execution-host",
        config["execution_host"],
        "--store-options-json",
        json.dumps(options, sort_keys=True),
    ]
    bundle = config.get("execution_bundle")
    if bundle is not None:
        bundle_sha = config.get("execution_bundle_sha256", "")
        if not Path(bundle).is_absolute() or not re.fullmatch(
            r"[0-9a-f]{64}", bundle_sha
        ):
            raise ValueError(
                "bare-host execution bundle requires an absolute path and SHA-256"
            )
        command = bundle_command(command, bundle, bundle_sha)
    properties = dict(config.get("platform_properties", {}))
    properties.update(
        {
            "OSFamily": "linux",
            "Arch": "amd64",
            "Pool": config["pool"],
            "container-image": "none",
            "workload-isolation-type": "none",
            "debug-executor-labels": "gpu-resource=" + host["resource_id"],
        }
    )
    # Secrets travel in remote headers, which BuildBuddy treats as sensitive.
    secret_overrides = []
    for name in config.get("worker_secret_env", []):
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            raise ValueError("invalid secret environment variable name")
        value = os.environ[name]
        if "\n" in value or "," in value:
            raise ValueError(
                "secret environment value cannot contain commas or newlines"
            )
        secret_overrides.append(name + "=" + value)
    request = {
        "repo": config["execution_repository"],
        "commit_sha": commit,
        "steps": [{"run": shlex.join(command)}],
        "timeout": "2h",
        "wait_until": "QUEUED",
        "platform_properties": properties,
        "env": config.get("worker_env", {}),
    }
    if bundle is not None:
        request["skip_auto_checkout"] = True
    if secret_overrides:
        request["remote_headers"] = [
            "x-buildbuddy-platform.secret-env-overrides=" + ",".join(secret_overrides)
        ]
    return request


REPORT_LINK_PLACEHOLDERS = ("repository", "target_sha", "target_branch")


def validate_report_links(links):
    """Only fixed https templates with identity placeholders may reach a comment."""
    if not isinstance(links, dict):
        raise ValueError("report_links must be an object of names to URL templates")
    validated = {}
    for name, template in links.items():
        if not isinstance(name, str) or not re.fullmatch(r"[a-z][a-z0-9_]*", name):
            raise ValueError("invalid report link name")
        if not isinstance(template, str) or not template.startswith("https://"):
            raise ValueError("report link %s must be an https URL" % name)
        if any(
            character.isspace() or character in "<>`" or ord(character) < 32
            for character in template
        ):
            raise ValueError("report link %s contains unsupported characters" % name)
        fields = []
        for _, field, specification, conversion in string.Formatter().parse(template):
            if field is None:
                continue
            # A nested spec such as {repository:{secret}} parses as the allowed
            # field, then fails or pads at format time. Accept plain fields only.
            if specification or conversion:
                raise ValueError(
                    "report link %s must use plain {placeholder} fields" % name
                )
            fields.append(field)
        unknown = set(fields) - set(REPORT_LINK_PLACEHOLDERS)
        if unknown:
            raise ValueError(
                "report link %s uses unsupported placeholders: %s"
                % (name, ", ".join(sorted(unknown)))
            )
        validated[name] = template
    return validated


def report_links(config, identity):
    values = {
        name: urllib.parse.quote(str(identity.get(name) or ""), safe="")
        for name in REPORT_LINK_PLACEHOLDERS
    }
    validated = validate_report_links(config.get("report_links", {}))
    return {name: template.format(**values) for name, template in validated.items()}


def run_prepared(
    plan_path,
    config,
    store,
    api,
    output_dir,
    current=lambda: True,
    poll_seconds=10,
    timeout_seconds=7200,
    current_check_seconds=0,
):
    """Schedule exactly the miss plan, retain failures, and always aggregate."""
    plan = read_json(plan_path)
    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    pending = {}
    artifacts = []
    errors = []
    started = time.monotonic()
    previous = {}

    def cancelled(signum, frame):
        raise InterruptedError("comparison coordinator cancelled")

    for sig in (signal.SIGTERM, signal.SIGINT):
        previous[sig] = signal.signal(sig, cancelled)
    try:
        if not current():
            raise RuntimeError("comparison superseded before worker submission")
        checked_at = time.monotonic()
        if plan["workers"]:
            prefix, plan_digest = upload_plan(store, plan_path)
            for worker in plan["workers"]:
                invocation = api.submit(
                    worker_request(config, plan, worker, prefix, plan_digest)
                )
                pending[invocation] = worker["worker_id"]
                write_json(output / "submissions.json", pending)
            reported_count, reported_at = 0, 0.0
            while pending:
                if time.monotonic() - started >= timeout_seconds:
                    raise TimeoutError(
                        "comparison workers exceeded the two-hour coordinator deadline"
                    )
                if time.monotonic() - checked_at >= current_check_seconds:
                    if not current():
                        raise RuntimeError(
                            "comparison superseded: PR head or target changed or PR closed"
                        )
                    checked_at = time.monotonic()
                for invocation, worker_id in list(pending.items()):
                    complete = api.complete(invocation)
                    if complete is None:
                        continue
                    raw = store.get(
                        prefix + f"workers/{worker_id}/worker-{worker_id}.json"
                    )
                    if raw is None:
                        errors.append(
                            f"worker {worker_id} ended without an artifact ({invocation})"
                        )
                    else:
                        path = output / f"worker-{worker_id}.json"
                        path.write_bytes(raw)
                        artifacts.append(path)
                    if not complete:
                        errors.append(
                            f"worker {worker_id} invocation failed ({invocation})"
                        )
                    del pending[invocation]
                if pending:
                    # Report changes, plus a heartbeat, without a line per poll.
                    now = time.monotonic()
                    if len(pending) != reported_count or now - reported_at >= 60:
                        print(f"Waiting for {len(pending)} GPU workers", flush=True)
                        reported_count, reported_at = len(pending), now
                    time.sleep(poll_seconds)
        # Completion can occur between periodic checks, including on a fully
        # cached run. Never publish a success based on a cached freshness check.
        if not current():
            raise RuntimeError("comparison superseded before report publication")
    except Exception as error:
        errors.append(str(error))
    finally:
        for invocation in pending:
            try:
                api.cancel(invocation)
            except Exception as error:
                errors.append(f"could not cancel worker {invocation}: {error}")
        for sig, handler in previous.items():
            signal.signal(sig, handler)
    # With errors, llm-cc keeps the analysis in analysis-report.json and
    # publishes a failure carrying both sets of errors.
    report = aggregate_report(
        output,
        plan=plan_path,
        workers=artifacts,
        errors=errors,
        executable=config.get("llm_cc"),
    )
    store_report(store, output, plan["identity"])
    return report


def _failed_artifact(worker_id, plan, errors, elapsed):
    return {
        "schema_version": 2,
        "identity": plan.get("identity") if isinstance(plan, dict) else None,
        "fingerprint": plan.get("fingerprint") if isinstance(plan, dict) else None,
        "worker_id": worker_id,
        "status": "failed",
        "results": {},
        "errors": errors,
        "elapsed_seconds": elapsed,
    }


def remote_worker(args):
    started = time.monotonic()

    def remaining():
        return max(0, WORKER_SECONDS - (time.monotonic() - started))

    # Credential providers used by boto can perform network I/O too.
    options = json.loads(args.store_options_json)
    with Deadline(WORKER_SECONDS):
        store = open_store(args.cache, **options)
    if not re.fullmatch(r"pipelines/[0-9a-f]{64}/", args.prefix):
        raise ValueError("invalid pipeline prefix")
    plan = {}
    errors = []
    with tempfile.TemporaryDirectory(prefix="llm-cc-worker-") as temporary:
        directory = Path(temporary)
        output = directory / "output"
        output.mkdir()
        artifact_path = output / f"worker-{args.worker_id}.json"
        try:
            # SIGALRM interrupts a blocked SDK retry on Linux.
            with Deadline(remaining()) as deadline:
                raw = store.get(args.prefix + "plan.json", max_bytes=PLAN_BYTES)
                deadline.check()
                if raw is None or hashlib.sha256(raw).hexdigest() != args.plan_sha256:
                    raise ValueError("preparation digest mismatch or missing plan")
                plan = json.loads(raw)
                if args.prefix != pipeline_prefix(plan["identity"]):
                    raise ValueError("preparation belongs to another pipeline")
                plan_path = directory / "plan.json"
                plan_path.write_bytes(raw)
                worker = next(
                    w for w in plan["workers"] if w["worker_id"] == args.worker_id
                )
                (directory / "blobs").mkdir()
                for key in worker["keys"]:
                    deadline.check()
                    item = plan["items"][key]
                    object_id = item["blob_id"]
                    # A blob can be no larger than the plan says.
                    content = store.get(
                        args.prefix + "blobs/" + object_id, max_bytes=item["size"]
                    )
                    deadline.check()
                    if content is None or blob_id(content, object_id) != object_id:
                        raise ValueError("missing or corrupt source blob")
                    (directory / "blobs" / object_id).write_bytes(content)
            # llm-cc verifies the host, holds the GPU lock, stores each result
            # as it lands and always writes its artifact. It cannot stop
            # mid-file, so the process gets a short grace period to finish.
            command = [
                args.llm_cc,
                "compare",
                "worker",
                "--plan",
                str(plan_path),
                "--worker-id",
                str(args.worker_id),
                "--output-dir",
                str(output),
                "--cache",
                args.cache,
                "--model",
                args.model,
                "--execution-host",
                args.execution_host,
                "--deadline-seconds",
                str(int(remaining())),
                "--progress",
                "never",
            ]
            if options:
                store_options = directory / "store-options.json"
                write_json(store_options, options)
                command += ["--store-options", str(store_options)]
            completed = subprocess.run(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=remaining() + 300,
            )
            if not artifact_path.is_file():
                errors.append(
                    "llm-cc compare worker exited %d without an artifact: %s"
                    % (
                        completed.returncode,
                        completed.stderr.decode("utf-8", "replace").strip()[-2000:],
                    )
                )
        except BaseException as error:
            errors.append(str(error) or type(error).__name__)
        if not artifact_path.is_file():
            write_json(
                artifact_path,
                _failed_artifact(
                    args.worker_id, plan, errors, time.monotonic() - started
                ),
            )
        result = read_json(artifact_path)
        # Failure reporting gets a small, bounded grace period after the main
        # deadline. Private caches and model data stay outside output.
        try:
            with Deadline(30) as cleanup:
                cleanup.check()
                store.put(
                    args.prefix
                    + f"workers/{args.worker_id}/worker-{args.worker_id}.json",
                    artifact_path.read_bytes(),
                )
        except DeadlineExceeded:
            return 1
        return 0 if result.get("status") == "complete" else 1


def prepare_plan(config, repo, head, identity, output):
    """Plan the comparison with `llm-cc compare prepare`; returns plan.json."""
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    identity_path = output / "identity.json"
    write_json(identity_path, identity)
    command = [
        config["llm_cc"],
        "compare",
        "prepare",
        "--repo",
        str(repo),
        "--head",
        head,
        "--target",
        identity["target_sha"],
        "--identity",
        str(identity_path),
        "--output-dir",
        str(output),
        "--cache",
        config["cache"],
        "--max-workers",
        str(config.get("max_workers", 4)),
        "--cache-concurrency",
        str(cache_concurrency(config)),
        "--refresh-days",
        str(config.get("refresh_days", 20)),
        "--expire-days",
        str(config.get("expire_days", 30)),
    ]
    if config.get("default_rules"):
        command += ["--default-rules", config["default_rules"]]
    with tempfile.TemporaryDirectory(prefix="llm-cc-prepare-") as temporary:
        presentation = Path(temporary) / "presentation.json"
        write_json(presentation, {"report_links": report_links(config, identity)})
        command += ["--presentation", str(presentation)]
        options = config.get("store_options", {})
        if options:
            store_options = Path(temporary) / "store-options.json"
            write_json(store_options, options)
            command += ["--store-options", str(store_options)]
        # The pinned scorer, model and scoring settings, one argument per line.
        command.append("@" + config["scoring_args"])
        try:
            completed = subprocess.run(
                command, stdout=subprocess.PIPE, stderr=subprocess.PIPE
            )
        except OSError as error:
            raise RuntimeError(
                "cannot run %s compare prepare: %s" % (command[0], error)
            ) from error
    if completed.returncode != 0:
        raise RuntimeError(
            "llm-cc compare prepare failed (exit %d): %s"
            % (
                completed.returncode,
                completed.stderr.decode("utf-8", "replace").strip(),
            )
        )
    return read_json(output / "plan.json")


def coordinate(args):
    output = Path(args.output_dir)
    identity = new_identity(args.repository, args.pipeline_id, args.head)
    config = {}
    try:
        config = read_json(args.config)
        github = GitHub(
            args.repository,
            os.environ.get(config.get("github_token_env", "GITHUB_TOKEN"), ""),
        )
        resolved = discover_identity(github, identity, args.branch, args.default_branch)
        if resolved is None:
            write_json(
                output / "skipped.json",
                {"reason": "branch has no current open PR", "identity": identity},
            )
            return 0
        identity = resolved
        store = open_store(config["cache"], **config.get("store_options", {}))
        # Fetch actual PR target, then full history if this checkout is shallow.
        fetch_target(args.repo, identity["target_sha"], identity.get("target_branch"))
        plan = prepare_plan(config, args.repo, args.head, identity, output)
        api = BuildBuddy(
            config.get("endpoint", "https://pawel.buildbuddy.io"),
            os.environ.get(config.get("api_key_env", "BUILDBUDDY_API_KEY"), ""),
        )
        report = run_prepared(
            output / "plan.json",
            config,
            store,
            api,
            output,
            current=lambda: github.current(plan["identity"]),
            # A completion poll is one BuildBuddy read per pending worker, so a
            # short interval stops the coordinator idling after workers finish.
            poll_seconds=2,
            # Anonymous GitHub REST reads share a 60/hour IP quota. Poll PR
            # freshness separately (20/hour), leaving room for discovery and
            # final checks while BuildBuddy completion remains responsive.
            current_check_seconds=10 if github.token else 180,
        )
        return 1 if report["status"] == "failed" else 0
    except Exception as error:
        aggregate_report(
            output,
            identity=identity,
            errors=[str(error)],
            executable=config.get("llm_cc") if isinstance(config, dict) else None,
        )
        return 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    stages = parser.add_subparsers(dest="stage", required=True)
    worker = stages.add_parser("remote-worker")
    for name in (
        "cache",
        "prefix",
        "plan-sha256",
        "llm-cc",
        "model",
        "execution-host",
    ):
        worker.add_argument("--" + name, required=True)
    worker.add_argument("--worker-id", type=int, required=True)
    worker.add_argument("--store-options-json", default="{}")
    coordinator = stages.add_parser("coordinate")
    for name in ("config", "repository", "head", "branch", "pipeline-id", "output-dir"):
        coordinator.add_argument("--" + name, required=True)
    coordinator.add_argument("--repo", default=".")
    coordinator.add_argument("--default-branch", default="main")
    args = parser.parse_args(argv)
    return remote_worker(args) if args.stage == "remote-worker" else coordinate(args)


if __name__ == "__main__":
    raise SystemExit(main())
