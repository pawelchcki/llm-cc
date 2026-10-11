"""Run real scorer acceptance in an isolated repository and cache.

    python3 -m tools.comparison.acceptance --executable /opt/llm-cc/bin/llm-cc \
        --model /models/model.gguf --scoring-args scoring.args --output-dir acceptance

The scoring arguments must pin the model and explicit execution settings.
Workers run sequentially, so one physical GPU can verify four-worker parity.
No repository publication, shared result cache or model download is needed.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import subprocess
import threading
import time
import uuid

from .common import read_json, write_json
from .store import open_store


def check_publication_store(location, options=None):
    """Check real conditional writes in the invocation's private namespace."""
    store = open_store(location, **(read_json(options) if options else {}))
    key = "publications/conditional.json"
    create_attempts = 8
    ready = threading.Barrier(create_attempts)

    def create(index):
        value = ("creator-%d" % index).encode()
        ready.wait(timeout=10)
        return value, store.put_if(key, value, None)

    with ThreadPoolExecutor(max_workers=create_attempts) as pool:
        creations = list(pool.map(create, range(create_attempts)))
    winners = [(value, token) for value, token in creations if token is not None]
    if len(winners) != 1:
        raise RuntimeError(
            "store allowed %d of %d simultaneous conditional creates"
            % (len(winners), create_attempts)
        )
    value, first = winners[0]
    if store.get_versioned(key) != (value, first):
        raise RuntimeError("conditional create did not retain its winner")
    second = store.put_if(key, b"second", first)
    if second is None or store.put_if(key, b"stale", first) is not None:
        raise RuntimeError("store did not enforce conditional replacement")
    if store.get_versioned(key) != (b"second", second):
        raise RuntimeError("conditional replacement changed its stored value")

    counter = "publications/counter.json"
    if store.put_if(counter, b"0", None) is None:
        raise RuntimeError("acceptance counter already exists")

    def increment(_):
        for _attempt in range(100):
            value, token = store.get_versioned(counter)
            if store.put_if(counter, str(int(value) + 1).encode(), token) is not None:
                return
        raise RuntimeError("conditional write retry budget exhausted")

    with ThreadPoolExecutor(max_workers=8) as pool:
        list(pool.map(increment, range(40)))
    stored = int(store.get(counter))
    if stored != 40:
        raise RuntimeError("concurrent store updates were lost: %d of 40" % stored)
    return {
        "conditional_create": True,
        "concurrent_create_attempts": create_attempts,
        "successful_creates": len(winners),
        "racing_create_rejected": True,
        "conditional_replace": True,
        "stale_replace_rejected": True,
        "concurrent_updates": 40,
        "stored_counter": stored,
    }


def run(args):
    root = args.output_dir.resolve()
    root.mkdir(parents=True, exist_ok=False)
    executable = str(args.executable.resolve())
    model = str(args.model.resolve())
    scoring = "@" + str(args.scoring_args.resolve())
    host = (
        ["--execution-host", str(args.execution_host.resolve())]
        if args.execution_host
        else []
    )
    store_options = (
        ["--store-options", str(args.store_options.resolve())]
        if args.store_options
        else []
    )
    store_prefix = (
        args.store.rstrip("/") + "/acceptance-" + uuid.uuid4().hex
        if args.store
        else str(root)
    )

    def cache(name):
        return store_prefix + "/store-" + name

    environment = os.environ | {
        "LLM_CC_CACHE_DIR": str(root / "native-cache"),
        "LLM_CC_ENTROPY_CACHE_DIR": str(root / "entropy-cache"),
    }

    def command(arguments, label, expected=0):
        started = time.monotonic()
        with (root / (label + ".stdout")).open("wb") as output:
            with (root / (label + ".stderr")).open("wb") as errors:
                result = subprocess.run(
                    [str(value) for value in arguments],
                    stdout=output,
                    stderr=errors,
                    env=environment,
                    timeout=args.timeout_seconds,
                )
        if result.returncode != expected:
            raise RuntimeError(
                "%s exited %d; see %s"
                % (label, result.returncode, root / (label + ".stderr"))
            )
        elapsed = time.monotonic() - started
        print("%s: %.3f seconds" % (label, elapsed), flush=True)
        return elapsed

    repo = root / "repository"
    repo.mkdir()

    def git(*arguments):
        return subprocess.check_output(
            ["git", "-C", str(repo), *arguments], text=True
        ).strip()

    def commit(message, files):
        for path, contents in files.items():
            file = repo / path
            file.parent.mkdir(parents=True, exist_ok=True)
            if contents is None:
                file.unlink()
            else:
                file.write_text(contents)
        git("add", ".")
        git(
            "-c",
            "user.name=Acceptance",
            "-c",
            "user.email=acceptance@example.com",
            "commit",
            "-qm",
            message,
        )
        return git("rev-parse", "HEAD")

    git("init", "-q", "-b", "main")
    source = "int square(int n) { return n * n; }\n"
    header = "int square(int n);\n"
    base = commit(
        "baseline",
        {
            ".llm-cc/rules.json": json.dumps(
                {"exclude": ["vendor/**"], "tests": ["tests/**"]}
            ),
            "src/base.c": source,
            "src/copy.c": source,
            "include/api.h": header,
            "vendor/upstream.c": "int excluded;\n",
            "README.md": "Acceptance fixture\n",
        },
    )
    head = commit(
        "pull request",
        {
            "src/new.py": "def cube(n):\n    return n * n * n\n",
            "tests/check.c": "int check(int n) { return n > 0; }\n",
        },
    )

    def comparison(name, workers, cache, reverse=False, revision=head):
        directory = root / name
        identity = root / (name + ".identity.json")
        write_json(identity, {"repository": "acceptance/fixture", "pipeline_id": name})
        command(
            [
                executable,
                "compare",
                "prepare",
                "--repo",
                repo,
                "--head",
                revision,
                "--target",
                base,
                "--identity",
                identity,
                "--cache",
                cache,
                *store_options,
                "--max-workers",
                workers,
                "--output-dir",
                directory,
                *host,
                scoring,
            ],
            name + "-prepare",
        )
        plan_path = directory / "plan.json"
        plan = read_json(plan_path)
        if reverse:
            for worker in plan["workers"]:
                worker["keys"].reverse()
            plan["workers"].reverse()
            write_json(plan_path, plan)
        results = dict(plan["hits"])
        artifacts = []
        for worker in plan["workers"]:
            worker_id = worker["worker_id"]
            output = directory / ("worker-%d" % worker_id)
            command(
                [
                    executable,
                    "compare",
                    "worker",
                    "--plan",
                    plan_path,
                    "--worker-id",
                    worker_id,
                    "--cache",
                    cache,
                    *store_options,
                    "--model",
                    model,
                    "--output-dir",
                    output,
                    "--deadline-seconds",
                    args.timeout_seconds,
                    *host,
                ],
                "%s-worker-%d" % (name, worker_id),
            )
            artifact = output / ("worker-%d.json" % worker_id)
            data = read_json(artifact)
            if data["status"] != "complete":
                raise RuntimeError("worker failed: " + json.dumps(data["errors"]))
            results.update(data["results"])
            artifacts += ["--worker", artifact]
        command(
            [
                executable,
                "compare",
                "aggregate",
                "--plan",
                plan_path,
                *artifacts,
                "--output-dir",
                directory / "report",
            ],
            name + "-aggregate",
        )
        report = read_json(directory / "report/report.json")
        if report["status"] != "complete":
            raise RuntimeError("incomplete comparison: " + name)
        return plan, results, report

    started = time.monotonic()
    one, expected, report = comparison("one", 1, cache("one"))
    four, actual, _ = comparison("four", 4, cache("four"))
    reversed_plan, reversed_results, _ = comparison(
        "reversed", 1, cache("reversed"), reverse=True
    )
    if len(one["workers"]) != 1 or len(four["workers"]) != 4:
        raise RuntimeError("fixture did not exercise one and four workers")
    if expected != actual or expected != reversed_results:
        raise RuntimeError("per-file results differ by worker count or input order")
    cold_seconds = time.monotonic() - started

    started = time.monotonic()
    warm, reused, warm_report = comparison("warm", 4, cache("one"))
    warm_seconds = time.monotonic() - started
    if warm["workers"] or warm["cache_stats"]["misses"] or reused != expected:
        raise RuntimeError("warm comparison did not reuse every result without workers")
    if warm_seconds >= args.warm_budget_seconds:
        raise RuntimeError("warm comparison exceeded the budget")
    if report["comparisons"] != warm_report["comparisons"]:
        raise RuntimeError("warm comparison changed category totals")

    renamed = commit(
        "rename, copy and category move",
        {
            "src/base.c": None,
            "src/renamed.c": source,
            "src/duplicate.c": source,
            "include/api.h": None,
            "tests/api.h": header,
        },
    )
    moved, moved_results, _ = comparison("renamed", 4, cache("one"), revision=renamed)
    if moved["workers"] or moved_results != expected:
        raise RuntimeError("rename, copy or category move caused inference")

    summary = {
        "store_prefix": store_prefix,
        "publication_store": check_publication_store(store_prefix, args.store_options),
        "scorer": one["scorer"],
        "model": one["model"],
        "scoring": one["scoring"],
        "fingerprint": one["fingerprint"],
        "unique_results": len(expected),
        "workers": {
            "one": len(one["workers"]),
            "four": len(four["workers"]),
            "reversed": len(reversed_plan["workers"]),
            "warm": len(warm["workers"]),
        },
        "exact_result_parity": True,
        "rename_copy_category_move_reused": True,
        "cold_seconds": cold_seconds,
        "warm_seconds": warm_seconds,
    }
    write_json(root / "acceptance.json", summary)
    print(json.dumps(summary, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--scoring-args", required=True, type=Path)
    parser.add_argument("--execution-host", type=Path)
    parser.add_argument("--store", help="optional S3 store; uses a unique test prefix")
    parser.add_argument("--store-options", type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--timeout-seconds", type=int, default=6600)
    parser.add_argument("--warm-budget-seconds", type=float, default=60)
    args = parser.parse_args()
    if args.timeout_seconds <= 0 or args.warm_budget_seconds <= 0:
        parser.error("timeout and warm budget must be positive")
    for name in (
        "executable",
        "model",
        "scoring_args",
        "execution_host",
        "store_options",
    ):
        path = getattr(args, name)
        if path is not None and not path.is_file():
            parser.error("%s must be an existing file" % name.replace("_", "-"))
    run(args)


if __name__ == "__main__":
    main()
