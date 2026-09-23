import argparse
import json
import os
import sys
import unittest
from pathlib import Path

from .cache import ResultCache, open_store
from .common import digest
from .github import API_URL, GitHub
from .inventory import validate_rules
from .pipeline import aggregate, compare, failure_report, prepare
from .worker import run_worker

# Stages that run around the comparison rather than producing its report;
# their failures print an error instead of writing a failure report.
RECIPE_COMMANDS = ("discover", "store-report", "publish", "ci")


def _json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def _common(parser):
    parser.add_argument("--repo", required=True)
    parser.add_argument("--head", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--identity", required=True)
    parser.add_argument("--profile", required=True)
    parser.add_argument("--rules", required=True)
    parser.add_argument("--cache", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--max-workers", type=int, default=4)
    parser.add_argument("--cache-concurrency", type=int, default=8)
    parser.add_argument("--refresh-days", type=int, default=20)
    parser.add_argument("--expire-days", type=int, default=30)
    parser.add_argument("--store-options")


def parser():
    result = argparse.ArgumentParser(prog="python -m tools.comparison")
    commands = result.add_subparsers(dest="command", required=True)
    preparation = commands.add_parser("prepare")
    _common(preparation)
    preparation.add_argument(
        "--config", help="recipe configuration JSON; its max_workers caps the plan"
    )
    worker = commands.add_parser("worker")
    worker.add_argument("--plan", required=True)
    worker.add_argument("--worker-id", type=int, required=True)
    worker.add_argument("--cache", required=True)
    worker.add_argument("--output-dir", required=True)
    worker.add_argument("--scorer", required=True)
    worker.add_argument("--model", required=True)
    worker.add_argument("--installed-root", required=True)
    worker.add_argument("--deadline-seconds", type=int, default=6600)
    worker.add_argument("--cache-concurrency", type=int, default=8)
    worker.add_argument("--store-options")
    aggregation = commands.add_parser("aggregate")
    aggregation.add_argument("--plan", required=True)
    aggregation.add_argument("--worker", action="append", default=[])
    aggregation.add_argument("--output-dir", required=True)
    comparison = commands.add_parser("compare")
    _common(comparison)
    comparison.add_argument("--scorer")
    comparison.add_argument("--model")
    comparison.add_argument("--installed-root")
    comparison.add_argument("--deadline-seconds", type=int, default=6600)
    commands.add_parser("test")
    discovery = commands.add_parser(
        "discover", help="resolve the PR identity, or skip a branch without one"
    )
    discovery.add_argument("--repository", required=True)
    discovery.add_argument("--head", required=True)
    discovery.add_argument("--branch", required=True)
    discovery.add_argument("--default-branch", default="main")
    discovery.add_argument("--pipeline-id", required=True)
    discovery.add_argument("--repo", help="checkout to fetch the actual target into")
    discovery.add_argument("--output", required=True)
    discovery.add_argument("--dotenv", help="append KEY=value outputs to this file")
    _github(discovery)
    storing = commands.add_parser(
        "store-report", help="store a report under its pipeline's prefix"
    )
    storing.add_argument("--cache", required=True)
    storing.add_argument("--report-dir", required=True)
    storing.add_argument("--store-options")
    publishing = commands.add_parser(
        "publish", help="publish a stored report as one ordered PR comment"
    )
    publishing.add_argument("--cache", required=True)
    publishing.add_argument("--store-options")
    publishing.add_argument("--repository", required=True)
    publishing.add_argument("--pipeline-id", required=True)
    publishing.add_argument("--head", required=True)
    publishing.add_argument("--ordinal", type=int, required=True)
    publishing.add_argument("--identity", help="discovery identity for failures")
    publishing.add_argument("--failure", help="publish this failure instead")
    publishing.add_argument("--comment-author")
    _github(publishing)
    adapters = commands.add_parser(
        "ci", help="generate CI jobs from a plan"
    ).add_subparsers(dest="adapter", required=True)
    child = adapters.add_parser("gitlab-child")
    child.add_argument("--plan")
    child.add_argument("--config", help="optional recipe configuration JSON")
    child.add_argument("--coordinator-image", help="overrides images.coordinator")
    child.add_argument("--output", required=True)
    child.add_argument("--skipped", metavar="REASON")
    child.add_argument("--cache")
    child.add_argument("--store-options")
    matrix = adapters.add_parser("github-matrix")
    matrix.add_argument("--plan", required=True)
    matrix.add_argument("--github-output", default=os.environ.get("GITHUB_OUTPUT"))
    return result


def _github(parser):
    parser.add_argument("--token-env", default="GITHUB_TOKEN")
    parser.add_argument(
        "--github-api-url", default=os.environ.get("GITHUB_API_URL") or API_URL
    )


def _run_recipe(args):
    from . import ci, discover, publish

    if args.command == "ci":
        return ci.run(args)
    options = _json(args.store_options) if getattr(args, "store_options", None) else {}
    if args.command == "store-report":
        prefix = publish.store_report(
            open_store(args.cache, **options), args.report_dir
        )
        print("Stored report under %s" % prefix)
        return 0
    github = GitHub(
        args.repository, os.environ.get(args.token_env, ""), args.github_api_url
    )
    if args.command == "discover":
        return discover.run(args, github)
    outcome, reason = publish.publish(
        open_store(args.cache, **options),
        github,
        repository=args.repository,
        pipeline_id=args.pipeline_id,
        head=args.head,
        ordinal=args.ordinal,
        identity=_json(args.identity) if args.identity else None,
        failure=args.failure,
        comment_author=args.comment_author,
    )
    print("publish: %s: %s" % (outcome, reason))
    return 0


def _run(args):
    if args.command == "test":
        root = Path(__file__).parents[2]
        suite = unittest.defaultTestLoader.discover(
            str(Path(__file__).parent), "test_*.py", str(root)
        )
        return (
            0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
        )
    if args.command == "aggregate":
        report = aggregate(args.plan, args.worker, args.output_dir)
        return 1 if report["status"] == "failed" else 0
    options = _json(args.store_options) if args.store_options else {}
    store = open_store(args.cache, **options)
    if args.command == "worker":
        artifact = run_worker(
            args.plan,
            args.worker_id,
            store,
            args.output_dir,
            args.scorer,
            args.model,
            args.installed_root,
            args.deadline_seconds,
            args.cache_concurrency,
        )
        return 0 if artifact["status"] == "complete" else 1
    cache = ResultCache(store, args.refresh_days, args.expire_days)
    max_workers = args.max_workers
    if getattr(args, "config", None):
        from .ci import load_config

        # `ci gitlab-child` rejects a plan with more workers than this.
        max_workers = min(max_workers, load_config(args.config)["max_workers"])
    kwargs = dict(
        repo=args.repo,
        head=args.head,
        target=args.target,
        identity=_json(args.identity),
        profile=_json(args.profile),
        rules=validate_rules(_json(args.rules)),
        cache=cache,
        output_dir=args.output_dir,
        max_workers=max_workers,
        cache_concurrency=args.cache_concurrency,
    )
    if args.command == "prepare":
        prepare(**kwargs)
        return 0
    report = compare(
        **kwargs,
        scorer=args.scorer,
        model=args.model,
        installed_root=args.installed_root,
        deadline_seconds=args.deadline_seconds,
    )
    return 0 if report["status"] != "failed" else 1


def main(argv=None):
    args = parser().parse_args(argv)
    if args.command in RECIPE_COMMANDS:
        try:
            return _run_recipe(args)
        except Exception as error:
            print("%s failed: %s" % (args.command, error), file=sys.stderr)
            return 1
    try:
        return _run(args)
    except Exception as error:
        identity, fingerprint = {}, None
        try:
            if getattr(args, "identity", None):
                identity = _json(args.identity)
            if getattr(args, "profile", None):
                profile = _json(args.profile)
                fingerprint = digest(
                    {"scoring": profile["scoring"], "build": profile["build"]}
                )
        except (OSError, ValueError, KeyError):
            pass
        failure_report(args.output_dir, identity, fingerprint, [str(error)])
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
