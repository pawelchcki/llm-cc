import argparse
import json
import os
import sys
import unittest
from pathlib import Path

from .github import API_URL, GitHub
from .store import open_store


def _json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def parser():
    result = argparse.ArgumentParser(prog="python -m tools.comparison")
    commands = result.add_subparsers(dest="command", required=True)
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
    limit = adapters.add_parser(
        "capacity", help="print --max-workers for llm-cc compare prepare"
    )
    limit.add_argument("--config", help="optional recipe configuration JSON")
    limit.add_argument("--requested", type=int, default=4)
    matrix = adapters.add_parser("github-matrix")
    matrix.add_argument("--plan", required=True)
    matrix.add_argument("--config", help="optional recipe configuration JSON")
    matrix.add_argument("--github-output", default=os.environ.get("GITHUB_OUTPUT"))
    return result


def _github(parser):
    parser.add_argument("--token-env", default="GITHUB_TOKEN")
    parser.add_argument(
        "--github-api-url", default=os.environ.get("GITHUB_API_URL") or API_URL
    )


def _test():
    root = Path(__file__).parents[2]
    suite = unittest.defaultTestLoader.discover(
        str(Path(__file__).parent), "test_*.py", str(root)
    )
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


def _run(args):
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


def main(argv=None):
    args = parser().parse_args(argv)
    if args.command == "test":
        return _test()
    # These stages run around the comparison, which `llm-cc compare` owns;
    # their failures print an error instead of writing a failure report.
    try:
        return _run(args)
    except Exception as error:
        print("%s failed: %s" % (args.command, error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
