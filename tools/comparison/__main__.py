import argparse
import json
import sys
import unittest
from pathlib import Path

from .cache import ResultCache, open_store
from .common import digest
from .pipeline import aggregate, compare, failure_report, prepare
from .worker import run_worker


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
    parser.add_argument("--refresh-days", type=int, default=20)
    parser.add_argument("--expire-days", type=int, default=30)
    parser.add_argument("--store-options")


def parser():
    result = argparse.ArgumentParser(prog="python -m tools.comparison")
    commands = result.add_subparsers(dest="command", required=True)
    _common(commands.add_parser("prepare"))
    worker = commands.add_parser("worker")
    worker.add_argument("--plan", required=True)
    worker.add_argument("--worker-id", type=int, required=True)
    worker.add_argument("--cache", required=True)
    worker.add_argument("--output-dir", required=True)
    worker.add_argument("--scorer", required=True)
    worker.add_argument("--model", required=True)
    worker.add_argument("--installed-root", required=True)
    worker.add_argument("--deadline-seconds", type=int, default=6600)
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
    return result


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
        )
        return 0 if artifact["status"] == "complete" else 1
    cache = ResultCache(store, args.refresh_days, args.expire_days)
    kwargs = dict(
        repo=args.repo,
        head=args.head,
        target=args.target,
        identity=_json(args.identity),
        profile=_json(args.profile),
        rules=_json(args.rules),
        cache=cache,
        output_dir=args.output_dir,
        max_workers=args.max_workers,
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
