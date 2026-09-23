"""Turn a preparation plan into CI jobs: zero to four GPU workers.

The GitLab adapter writes a child pipeline and the GitHub adapter a job matrix.
Both take the scorer image from the plan's fingerprinted profile, so a GPU job
can only run the exact image its cached results are keyed by. A fully cached
plan produces no GPU job at all.
"""

from __future__ import annotations

import json
from pathlib import PurePosixPath
import re
import shlex

from .common import read_json

IMAGE = re.compile(r"[^\s@]+@sha256:[0-9a-f]{64}")
# Where the recipe's scorer image installs the scorer and its model.
SCORER_PATHS = {
    "executable": "/opt/llm-cc/bin/llm-cc",
    "installed_root": "/opt/llm-cc",
    "model": "/models/model.gguf",
}
DEFAULTS = {
    "max_workers": 4,
    "cache_concurrency": 8,
    "worker_timeout": "120m",
    "aggregate_timeout": "30m",
    "artifact_expiry": "14 days",
}


def image_reference(value, role):
    if not isinstance(value, str) or not IMAGE.fullmatch(value):
        raise ValueError(
            "%s image must be pinned by digest as name@sha256:<64 hex>" % role
        )
    return value


def _strings(value, name):
    if not isinstance(value, list) or not all(
        isinstance(item, str) and item for item in value
    ):
        raise ValueError("%s must be a list of non-empty strings" % name)
    return value


def load_config(path=None):
    """Read and validate the optional provider-neutral recipe configuration."""
    config = {"schema_version": 1} if path is None else read_json(path)
    if not isinstance(config, dict) or config.get("schema_version") != 1:
        raise ValueError("CI configuration needs schema_version 1")
    images = config.setdefault("images", {})
    if not isinstance(images, dict):
        raise ValueError("images must map roles to digest references")
    for role in ("coordinator", "scorer"):
        if images.get(role) is not None:
            image_reference(images[role], role)
    scorer = config.setdefault("scorer", {})
    if not isinstance(scorer, dict):
        raise ValueError("scorer must be an object of paths")
    for name, default in SCORER_PATHS.items():
        if not isinstance(scorer.setdefault(name, default), str) or not scorer[
            name
        ].startswith("/"):
            raise ValueError("scorer %s must be an absolute path" % name)
    for name, value in DEFAULTS.items():
        config.setdefault(name, value)
    if type(config["max_workers"]) is not int or not 1 <= config["max_workers"] <= 4:
        raise ValueError("max_workers must be between 1 and 4")
    if (
        type(config["cache_concurrency"]) is not int
        or not 1 <= config["cache_concurrency"] <= 64
    ):
        raise ValueError("cache_concurrency must be between 1 and 64")
    gitlab = config.setdefault("gitlab", {})
    gitlab.setdefault("prepare_job", "comparison-prepare")
    _strings(gitlab.setdefault("cpu_tags", []), "gitlab.cpu_tags")
    _strings(gitlab.setdefault("gpu_tags", []), "gitlab.gpu_tags")
    return config


def _scorer_image(plan, config):
    image = image_reference(plan["profile"]["build"].get("execution_image"), "scorer")
    configured = config["images"].get("scorer")
    if configured is not None and configured != image:
        raise ValueError(
            "configured scorer image differs from the plan's fingerprinted profile"
        )
    return image


def _workers(plan, config):
    ids = [worker["worker_id"] for worker in plan["workers"]]
    if any(type(value) is not int or not 0 <= value < 4 for value in ids) or len(
        set(ids)
    ) != len(ids):
        raise ValueError("plan has invalid worker IDs")
    if len(ids) > config["max_workers"]:
        raise ValueError(
            "plan needs %d workers but the configuration allows %d"
            % (len(ids), config["max_workers"])
        )
    return sorted(ids)


def _relative(path):
    value = PurePosixPath(path)
    if value.is_absolute() or ".." in value.parts or not value.parts:
        raise ValueError("artifact paths must be relative to the project directory")
    return value.as_posix()


def _store_arguments(store, options):
    """Store flags for generated jobs; options travel inline, never secrets."""
    if not isinstance(store, str) or not store:
        raise ValueError("generated jobs need a store location")
    # Credential keys come in any case (AWS_SECRET_ACCESS_KEY, SessionToken).
    if not isinstance(options, dict) or any(
        word in key.lower()
        for key in options
        for word in ("secret", "access_key", "token")
    ):
        raise ValueError(
            "store options must not carry credentials; use CI secret variables"
        )
    arguments = ["--cache", store]
    setup = []
    if options:
        setup.append(
            "printf '%%s' %s > store-options.json"
            % shlex.quote(json.dumps(options, sort_keys=True))
        )
        arguments += ["--store-options", "store-options.json"]
    return arguments, setup


def _tags(tags):
    return {"tags": tags} if tags else {}


def _command(*arguments):
    return shlex.join(["python3", "-m", "tools.comparison", *arguments])


def _coordinator_image(config, override):
    return image_reference(
        override or config["images"].get("coordinator"), "coordinator"
    )


def gitlab_child(plan, plan_path, config, store, store_options=None, coordinator=None):
    """A child pipeline: one GPU job per planned worker, then aggregation."""
    plan_path = _relative(plan_path)
    gitlab = config["gitlab"]
    coordinator = _coordinator_image(config, coordinator)
    store, setup = _store_arguments(store, store_options or {})
    prepared = {"pipeline": "$PARENT_PIPELINE_ID", "job": gitlab["prepare_job"]}
    child = {"stages": ["score", "aggregate"]}
    worker_jobs = []
    worker_paths = []
    scorer = _scorer_image(plan, config) if plan["workers"] else None
    for worker_id in _workers(plan, config):
        name = "comparison-worker-%d" % worker_id
        output = "comparison/workers/%d" % worker_id
        worker_jobs.append(name)
        worker_paths.append("%s/worker-%d.json" % (output, worker_id))
        command = _command(
            "worker",
            "--plan",
            plan_path,
            "--worker-id",
            str(worker_id),
            *store,
            "--output-dir",
            output,
            "--scorer",
            config["scorer"]["executable"],
            "--model",
            config["scorer"]["model"],
            "--installed-root",
            config["scorer"]["installed_root"],
            "--cache-concurrency",
            str(config["cache_concurrency"]),
        )
        child[name] = {
            "stage": "score",
            "image": scorer,
            **_tags(gitlab["gpu_tags"]),
            "interruptible": True,
            "timeout": config["worker_timeout"],
            "variables": {"GIT_STRATEGY": "none"},
            "needs": [prepared],
            "script": setup + [command],
            "artifacts": {
                "when": "always",
                "expire_in": config["artifact_expiry"],
                "paths": [output + "/"],
            },
        }
    worker_arguments = []
    for path in worker_paths:
        worker_arguments += ["--worker", path]
    # Every worker writes its own directory and preparation artifacts hold no
    # worker placeholders, so downloads cannot clobber a completed result.
    child["comparison-aggregate"] = {
        "stage": "aggregate",
        "image": coordinator,
        **_tags(gitlab["cpu_tags"]),
        "when": "always",
        "interruptible": True,
        "timeout": config["aggregate_timeout"],
        "variables": {"GIT_STRATEGY": "none"},
        "needs": [prepared]
        + [{"job": name, "artifacts": True} for name in worker_jobs],
        "script": setup
        + [
            "status=0",
            _command(
                "aggregate",
                "--plan",
                plan_path,
                *worker_arguments,
                "--output-dir",
                "comparison/report",
            )
            + " || status=$?",
            _command("store-report", *store, "--report-dir", "comparison/report"),
            'exit "$status"',
        ],
        "artifacts": {
            "when": "always",
            "expire_in": config["artifact_expiry"],
            "paths": ["comparison/report/"],
        },
    }
    return child


def gitlab_skipped(config, reason, coordinator=None):
    """GitLab rejects an empty child pipeline, so a skip still runs one job."""
    return {
        "comparison-skipped": {
            "image": _coordinator_image(config, coordinator),
            **_tags(config["gitlab"]["cpu_tags"]),
            "interruptible": True,
            "variables": {"GIT_STRATEGY": "none"},
            "script": ["echo " + shlex.quote(reason)],
        }
    }


def write_yaml(path, document):
    # JSON is valid YAML, so generated pipelines need no YAML library.
    with open(path, "w", encoding="utf-8") as stream:
        stream.write("# Generated by python3 -m tools.comparison ci; do not edit.\n")
        json.dump(document, stream, indent=2)
        stream.write("\n")


def github_matrix(plan):
    """Outputs for a dynamic matrix; an empty matrix must skip the GPU job."""
    ids = [worker["worker_id"] for worker in plan["workers"]]
    if any(type(value) is not int or not 0 <= value < 4 for value in ids):
        raise ValueError("plan has invalid worker IDs")
    outputs = {
        "matrix": json.dumps(
            {"include": [{"worker_id": value} for value in sorted(ids)]},
            separators=(",", ":"),
        ),
        "has_workers": "true" if ids else "false",
    }
    if ids:
        outputs["scorer_image"] = image_reference(
            plan["profile"]["build"].get("execution_image"), "scorer"
        )
    return outputs


def run(args):
    if args.adapter == "gitlab-child":
        config = load_config(args.config)
        if args.skipped is not None:
            document = gitlab_skipped(config, args.skipped, args.coordinator_image)
        else:
            if not args.plan or not args.cache:
                raise ValueError(
                    "gitlab-child needs --plan and --cache unless --skipped"
                )
            options = read_json(args.store_options) if args.store_options else {}
            document = gitlab_child(
                read_json(args.plan),
                args.plan,
                config,
                args.cache,
                options,
                args.coordinator_image,
            )
        write_yaml(args.output, document)
        workers = sum(name.startswith("comparison-worker-") for name in document)
        print("Generated %s with %d GPU worker job(s)." % (args.output, workers))
        return 0
    outputs = github_matrix(read_json(args.plan))
    lines = "".join("%s=%s\n" % item for item in outputs.items())
    if args.github_output:
        with open(args.github_output, "a", encoding="utf-8") as stream:
            stream.write(lines)
    print(lines, end="")
    return 0
