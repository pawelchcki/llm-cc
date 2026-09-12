"""CPU coordinator and remote-worker transport for BuildBuddy.

Run uses the public enterprise API; cancellation uses CancelExecutions from
BuildBuddyService. Credentials remain environment inputs, never plan artifacts.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import tempfile
import time
import urllib.parse
import urllib.request

from .cache import ResultCache, open_store
from .common import (
    canonical_bytes,
    digest,
    read_json,
    validate_execution_policy,
    write_json,
)
from .deadline import Deadline, DeadlineExceeded
from .pipeline import aggregate, prepare


def _request(
    url,
    token,
    payload=None,
    token_header="Authorization",
    content_type="application/json",
):
    headers = {"Accept": "application/json", "Content-Type": content_type}
    if token:
        headers[token_header] = (
            "Bearer " if token_header == "Authorization" else ""
        ) + token
    data = (
        payload
        if isinstance(payload, bytes)
        else canonical_bytes(payload)
        if payload is not None
        else None
    )
    request = urllib.request.Request(url, data=data, headers=headers)
    with urllib.request.urlopen(request, timeout=45) as response:
        body = response.read()
    return json.loads(body) if content_type == "application/json" else body


class GitHub:
    def __init__(self, repository, token=""):
        if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
            raise ValueError("repository must be owner/name")
        self.repository, self.token = repository, token

    def get(self, suffix):
        return _request(
            "https://api.github.com/repos/" + self.repository + suffix, self.token
        )

    def discover(self, head, branch, default_branch):
        if branch == default_branch:
            current = self.get(
                "/commits/" + urllib.parse.quote(default_branch, safe="")
            )["sha"]
            if current != head:
                raise ValueError("default-branch push was superseded")
            return {
                "head_sha": head,
                "target_sha": head,
                "target_branch": default_branch,
                "pr_number": None,
            }
        pulls = []
        page = 1
        while True:
            batch = self.get(f"/commits/{head}/pulls?per_page=100&page={page}")
            pulls.extend(
                p
                for p in batch
                if p["state"] == "open"
                and p["head"]["sha"] == head
                and p["head"]["ref"] == branch
                and p["base"]["repo"]["full_name"] == self.repository
            )
            if len(batch) < 100:
                break
            page += 1
        if not pulls:
            return None
        if len(pulls) != 1:
            raise ValueError(
                "multiple open PRs match this head; provide an unambiguous PR identity"
            )
        pull = self.get(f"/pulls/{pulls[0]['number']}")
        if pull["state"] != "open" or pull["head"]["sha"] != head:
            raise ValueError("PR changed during discovery")
        return {
            "head_sha": head,
            "target_sha": pull["base"]["sha"],
            "target_branch": pull["base"]["ref"],
            "pr_number": pull["number"],
        }

    def current(self, identity):
        if identity["pr_number"] is None:
            return (
                self.get(
                    "/commits/" + urllib.parse.quote(identity["target_branch"], safe="")
                )["sha"]
                == identity["head_sha"]
            )
        pull = self.get(f"/pulls/{identity['pr_number']}")
        return (
            pull["state"] == "open"
            and pull["head"]["sha"] == identity["head_sha"]
            and pull["base"]["sha"] == identity["target_sha"]
            and pull["base"]["ref"] == identity["target_branch"]
        )


class BuildBuddy:
    def __init__(self, endpoint, api_key):
        if not endpoint.startswith("https://"):
            raise ValueError("BuildBuddy endpoint must use HTTPS")
        self.endpoint, self.api_key = endpoint.rstrip("/"), api_key

    def api(self, method, payload):
        return _request(
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
        return _request(
            self.endpoint + "/rpc/BuildBuddyService/CancelExecutions",
            self.api_key,
            b"\x12" + bytes([len(encoded)]) + encoded,
            "x-buildbuddy-api-key",
            "application/proto",
        )


def _pipeline_prefix(identity):
    # Pipeline IDs are opaque input, never filesystem paths.
    return (
        "pipelines/" + digest([identity["repository"], identity["pipeline_id"]]) + "/"
    )


def upload_plan(store, plan_path):
    plan_path = Path(plan_path)
    plan = read_json(plan_path)
    prefix = _pipeline_prefix(plan["identity"])
    for worker in plan["workers"]:
        for key in worker["keys"]:
            item = plan["items"][key]
            content = (plan_path.parent / item["blob"]).read_bytes()
            if hashlib.sha256(content).hexdigest() != item["content_sha256"]:
                raise ValueError("preparation blob changed before upload")
            store.put(prefix + item["blob"], content)
    payload = canonical_bytes(plan)
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


def worker_request(config, plan, worker, prefix, plan_digest):
    validate_execution_policy(plan["profile"])
    image = config["execution_image"]
    build = plan["profile"]["build"]
    bare_host = image == "none" and bool(build.get("execution_host"))
    if image != build["execution_image"] or not (
        bare_host or re.fullmatch(r"[^\s]+@sha256:[0-9a-f]{64}", image)
    ):
        raise ValueError(
            "worker execution image does not match the immutable scoring profile"
        )
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
        "--scorer",
        config["scorer"],
        "--model",
        config["model"],
        "--installed-root",
        config["installed_root"],
        "--store-options-json",
        json.dumps(options, sort_keys=True),
    ]
    bundle = config.get("execution_bundle")
    if bundle is not None:
        bundle_sha = config.get("execution_bundle_sha256", "")
        if (
            not bare_host
            or not Path(bundle).is_absolute()
            or not re.fullmatch(r"[0-9a-f]{64}", bundle_sha)
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
            "container-image": image,
        }
    )
    if bare_host:
        properties["workload-isolation-type"] = "none"
        properties["debug-executor-labels"] = (
            "gpu-resource=" + build["execution_host"]["resource_id"]
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
    from .pipeline import failure_report

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
                    print(f"Waiting for {len(pending)} GPU workers", flush=True)
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
    report = aggregate(plan_path, artifacts, output)
    if errors:
        write_json(output / "analysis-report.json", report)
        report = failure_report(
            output,
            plan["identity"],
            plan["fingerprint"],
            errors + report.get("errors", []),
        )
    prefix = _pipeline_prefix(plan["identity"])
    for name in ("report.json", "report.md", "comment.md", "publication.json"):
        store.put(prefix + name, (output / name).read_bytes())
    return report


def remote_worker(args):
    from .worker import run_worker

    started = time.monotonic()
    # Credential providers used by boto can perform network I/O too.
    with Deadline(6600):
        store = open_store(args.cache, **json.loads(args.store_options_json))
    if not re.fullmatch(r"pipelines/[0-9a-f]{64}/", args.prefix):
        raise ValueError("invalid pipeline prefix")
    plan = {}
    with tempfile.TemporaryDirectory(prefix="llm-cc-worker-") as temporary:
        directory = Path(temporary)
        output = directory / "output"
        output.mkdir()
        try:
            # This includes object transport, validation, scoring, and cache
            # publication. SIGALRM interrupts a blocked SDK retry on Linux.
            with Deadline(max(0, 6600 - (time.monotonic() - started))) as deadline:
                raw = store.get(args.prefix + "plan.json")
                deadline.check()
                if raw is None or hashlib.sha256(raw).hexdigest() != args.plan_sha256:
                    raise ValueError("preparation digest mismatch or missing plan")
                plan = json.loads(raw)
                if args.prefix != _pipeline_prefix(plan["identity"]):
                    raise ValueError("preparation belongs to another pipeline")
                plan_path = directory / "plan.json"
                plan_path.write_bytes(raw)
                worker = next(
                    w for w in plan["workers"] if w["worker_id"] == args.worker_id
                )
                for key in worker["keys"]:
                    deadline.check()
                    item = plan["items"][key]
                    relative = "blobs/" + item["content_sha256"]
                    if item["blob"] != relative or not re.fullmatch(
                        r"blobs/[0-9a-f]{64}", relative
                    ):
                        raise ValueError("invalid preparation blob path")
                    content = store.get(args.prefix + relative)
                    deadline.check()
                    if (
                        content is None
                        or hashlib.sha256(content).hexdigest() != item["content_sha256"]
                    ):
                        raise ValueError("missing or corrupt source blob")
                    destination = directory / relative
                    destination.parent.mkdir(exist_ok=True)
                    destination.write_bytes(content)
                result = run_worker(
                    plan_path,
                    args.worker_id,
                    store,
                    output,
                    args.scorer,
                    args.model,
                    args.installed_root,
                    deadline_seconds=max(0, 6600 - (time.monotonic() - started)),
                )
                deadline.check()
        except BaseException as error:
            result = {
                "schema_version": 1,
                "identity": plan.get("identity") if isinstance(plan, dict) else None,
                "fingerprint": plan.get("fingerprint")
                if isinstance(plan, dict)
                else None,
                "worker_id": args.worker_id,
                "status": "failed",
                "results": {},
                "errors": [str(error)],
                "elapsed_seconds": time.monotonic() - started,
            }
            write_json(output / f"worker-{args.worker_id}.json", result)
        # Failure reporting gets a small, bounded grace period after the main
        # deadline. Private caches and model data stay outside output.
        try:
            with Deadline(30) as cleanup:
                artifact_path = output / f"worker-{args.worker_id}.json"
                paths = [artifact_path] if artifact_path.is_file() else []
                paths.extend(
                    path for path in sorted(output.rglob("*")) if path != artifact_path
                )
                for path in paths:
                    if path.is_file() and not path.is_symlink():
                        cleanup.check()
                        store.put(
                            args.prefix
                            + f"workers/{args.worker_id}/"
                            + path.relative_to(output).as_posix(),
                            path.read_bytes(),
                        )
        except DeadlineExceeded:
            return 1
        return 0 if result["status"] == "complete" else 1


def coordinate(args):
    from .pipeline import failure_report

    output = Path(args.output_dir)
    identity = {
        "repository": args.repository,
        "pipeline_id": args.pipeline_id,
        "head_sha": args.head,
        "target_sha": None,
        "base_sha": None,
        "target_branch": None,
        "pr_number": None,
        "started_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
    }
    fingerprint = "0" * 64
    try:
        config = read_json(args.config)
        github = GitHub(
            args.repository,
            os.environ.get(config.get("github_token_env", "GITHUB_TOKEN"), ""),
        )
        resolved = github.discover(args.head, args.branch, args.default_branch)
        if resolved is None:
            write_json(
                output / "skipped.json",
                {"reason": "branch has no current open PR", "identity": identity},
            )
            return 0
        identity.update(resolved)
        profile = read_json(config["profile"])
        fingerprint = digest({"scoring": profile["scoring"], "build": profile["build"]})
        store = open_store(config["cache"], **config.get("store_options", {}))
        cache = ResultCache(
            store, config.get("refresh_days", 20), config.get("expire_days", 30)
        )
        # Fetch actual PR target, then full history if this checkout is shallow.
        subprocess.run(
            ["git", "-C", args.repo, "fetch", "origin", identity["target_sha"]],
            check=True,
        )
        shallow = subprocess.check_output(
            ["git", "-C", args.repo, "rev-parse", "--is-shallow-repository"], text=True
        ).strip()
        if shallow == "true":
            subprocess.run(
                ["git", "-C", args.repo, "fetch", "--unshallow", "origin"], check=True
            )
        plan = prepare(
            args.repo,
            args.head,
            identity["target_sha"],
            identity,
            profile,
            read_json(config["rules"]),
            cache,
            output,
            config.get("max_workers", 4),
        )
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
            # Anonymous GitHub REST reads share a 60/hour IP quota. Poll PR
            # freshness separately (20/hour), leaving room for discovery and
            # final checks while BuildBuddy completion remains responsive.
            current_check_seconds=10 if github.token else 180,
        )
        return 1 if report["status"] == "failed" else 0
    except Exception as error:
        failure_report(output, identity, fingerprint, [str(error)])
        return 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    stages = parser.add_subparsers(dest="stage", required=True)
    worker = stages.add_parser("remote-worker")
    for name in ("cache", "prefix", "plan-sha256", "scorer", "model", "installed-root"):
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
