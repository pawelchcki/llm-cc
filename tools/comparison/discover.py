"""Resolve the comparison identity for one push before any scoring work.

Default-branch pushes compare the head with itself and seed the baseline cache.
Any other branch resolves its single current open pull request and that PR's
actual target; a branch without one is skipped. Discovery only reads GitHub.
"""

from __future__ import annotations

from datetime import datetime, timezone
import json
from pathlib import Path
import re
import subprocess

from .common import write_json

SKIP_VARIABLE = "COMPARISON_SKIP"


def new_identity(repository, pipeline_id, head):
    """An identity whose target fields stay null until discovery resolves them."""
    return {
        "repository": repository,
        "pipeline_id": pipeline_id,
        "head_sha": head,
        "target_sha": None,
        "base_sha": None,
        "target_branch": None,
        "pr_number": None,
        "started_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
    }


def discover_identity(github, identity, branch, default_branch):
    """The resolved identity, or None when this branch has no current PR."""
    resolved = github.discover(identity["head_sha"], branch, default_branch)
    return None if resolved is None else dict(identity, **resolved)


def fetch_target(repo, target_sha, target_branch=None, remote="origin"):
    """Make the actual target and full history available to inventory."""
    repo = str(repo)
    fetched = subprocess.run(["git", "-C", repo, "fetch", remote, target_sha])
    if fetched.returncode != 0:
        if target_branch is None:
            raise subprocess.CalledProcessError(fetched.returncode, fetched.args)
        # Some mirrors refuse fetching an object by name; the target branch
        # must still contain the exact commit discovery resolved.
        subprocess.run(
            [
                "git",
                "-C",
                repo,
                "fetch",
                remote,
                "+refs/heads/%s:refs/remotes/%s/%s"
                % (target_branch, remote, target_branch),
            ],
            check=True,
        )
        subprocess.run(
            ["git", "-C", repo, "cat-file", "-e", target_sha + "^{commit}"], check=True
        )
    shallow = subprocess.check_output(
        ["git", "-C", repo, "rev-parse", "--is-shallow-repository"], text=True
    ).strip()
    if shallow == "true":
        subprocess.run(["git", "-C", repo, "fetch", "--unshallow", remote], check=True)


def write_dotenv(path, values):
    """Append KEY=value lines for GitLab dotenv reports or $GITHUB_OUTPUT."""
    lines = []
    for name, value in values.items():
        text = "" if value is None else str(value)
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name) or not re.fullmatch(
            r"[A-Za-z0-9._/-]*", text
        ):
            raise ValueError("unsafe dotenv entry: %s" % name)
        lines.append("%s=%s\n" % (name, text))
    with open(path, "a", encoding="utf-8") as stream:
        stream.write("".join(lines))


def run(args, github):
    if not re.fullmatch(r"[0-9a-f]{40}", args.head):
        raise ValueError("--head must be a full lowercase Git commit SHA")
    output = Path(args.output)
    unresolved = new_identity(args.repository, args.pipeline_id, args.head)
    identity = discover_identity(github, unresolved, args.branch, args.default_branch)
    if identity is None:
        write_json(
            output.with_name("skipped.json"),
            {"reason": "branch has no current open PR", "identity": unresolved},
        )
        if args.dotenv:
            write_dotenv(args.dotenv, {SKIP_VARIABLE: 1})
        print("No current open pull request for %s; skipping." % args.branch)
        return 0
    if args.repo is not None:
        fetch_target(args.repo, identity["target_sha"], identity["target_branch"])
    write_json(output, identity)
    if args.dotenv:
        outputs = {
            SKIP_VARIABLE: 0,
            "COMPARISON_PR_NUMBER": identity["pr_number"],
            "COMPARISON_TARGET_SHA": identity["target_sha"],
        }
        # A default-branch run has no PR number; omit it rather than emit an
        # empty dotenv value.
        write_dotenv(args.dotenv, {k: v for k, v in outputs.items() if v is not None})
    print(json.dumps(identity, sort_keys=True))
    return 0
