"""Submit a CPU coordinator using the installed, checksum-pinned host package."""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess

from .buildbuddy import BuildBuddy, bundle_command, coordinate
from .common import read_json


def runner_metadata(name, artifact_directory=None, environment=None):
    """Read only allowlisted metadata from the runner's credential-bearing rc."""
    if name not in {"PARENT_INVOCATION_ID", "BRANCH_NAME", "COMMIT_SHA"}:
        raise ValueError("unsupported BuildBuddy runner metadata name")
    environment = os.environ if environment is None else environment
    artifacts = artifact_directory or environment.get("BUILDBUDDY_ARTIFACTS_DIRECTORY")
    if not artifacts:
        raise ValueError("BuildBuddy runner metadata requires its artifact directory")
    artifacts = Path(artifacts)
    if not artifacts.is_absolute() or artifacts.parent.name != "artifacts":
        raise ValueError(
            "BuildBuddy artifact directory must be an absolute workspace/artifacts/command directory"
        )
    rc = artifacts.parents[1] / "buildbuddy.bazelrc"
    try:
        lines = rc.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError):
        raise ValueError("cannot read BuildBuddy-generated runner metadata") from None
    candidates = []
    for line in lines:
        match = re.fullmatch(
            r"[ \t]*common[ \t]+--build_metadata=" + name + r"=(.*?)[ \t]*", line
        )
        if match:
            candidates.append(match.group(1))
    if len(candidates) != 1:
        raise ValueError(
            "BuildBuddy runner metadata must contain exactly one identity for requested field"
        )
    value = candidates[0]
    if not value or any(
        character.isspace() or ord(character) < 32 or ord(character) == 127
        for character in value
    ):
        raise ValueError("BuildBuddy runner metadata has an invalid value")
    if name == "COMMIT_SHA" and not re.fullmatch(r"[0-9a-fA-F]{40}", value):
        raise ValueError("BuildBuddy COMMIT_SHA must be a full Git SHA")
    return value


CI_MERGE_COMMITTER = "ci-runner@buildbuddy.io"


def _git(repo, *args):
    return subprocess.run(
        ["git", "-C", str(repo), *args],
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    ).stdout.strip()


def resolve_checkout(repo=".", environment=None, default_branch=None):
    """Derive the analysed head and branch from whatever the runner provides."""
    environment = os.environ if environment is None else environment
    head = environment.get("GIT_COMMIT") or environment.get("COMMIT_SHA")
    if not head:
        try:
            head = runner_metadata("COMMIT_SHA", environment=environment)
        except ValueError:
            head = None
    if not head:
        head = _git(repo, "rev-parse", "HEAD")
    if not re.fullmatch(r"[0-9a-fA-F]{40}", head):
        raise ValueError("checkout head must be a full Git commit SHA")
    head = head.lower()
    # BuildBuddy checks pull requests out at a synthetic merge commit whose
    # first parent is the actual PR head. Compare the head the PR proposes.
    # The committer email alone is attacker-controlled, so require the merge
    # shape too: an ordinary commit forged with that email keeps its own SHA
    # instead of silently unwrapping to its parent and losing PR discovery.
    committer, parents = (
        _git(repo, "show", "-s", "--format=%ce%n%P", head).split("\n", 1) + [""]
    )[:2]
    if committer == CI_MERGE_COMMITTER and len(parents.split()) >= 2:
        head = _git(repo, "rev-parse", head + "^1")
    branch = environment.get("GIT_BRANCH") or _git(repo, "branch", "--show-current")
    if not branch:
        branch = runner_metadata("BRANCH_NAME", environment=environment)
    default = default_branch or environment.get("GIT_REPO_DEFAULT_BRANCH") or "main"
    return head, branch, default


def parent_invocation_id(artifact_directory=None, environment=None):
    """Read and validate the parent UUID, allowing an explicit environment input."""
    environment = os.environ if environment is None else environment
    invocation = environment.get("BUILDBUDDY_INVOCATION_ID")
    if invocation is None:
        invocation = runner_metadata(
            "PARENT_INVOCATION_ID", artifact_directory, environment
        )
    if not re.fullmatch(
        r"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}", invocation
    ):
        raise ValueError("BuildBuddy parent invocation ID must be a UUID")
    return invocation.lower()


def coordinator_request(config, config_path, repository, head, branch, default_branch):
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("repository must be owner/name")
    if not re.fullmatch(r"[0-9a-f]{40}", head):
        raise ValueError("head must be a full Git commit SHA")
    if config.get("execution_image") != "none" or not Path(config_path).is_absolute():
        raise ValueError(
            "Bazzite coordinator requires a bare-host configuration at an absolute path"
        )
    command = bundle_command(
        [
            "python3",
            "-m",
            "tools.comparison.submit_bazzite",
            "run-coordinator",
            "--config",
            config_path,
            "--repository",
            repository,
            "--head",
            head,
            "--branch",
            branch,
            "--default-branch",
            default_branch,
        ],
        config["execution_bundle"],
        config["execution_bundle_sha256"],
    )
    # Run already injects a group-scoped BUILDBUDDY_API_KEY into its runner.
    # Local API credentials authenticate the submission only. Public GitHub PR
    # discovery needs no token; private access belongs in trusted runner secrets.
    return {
        "repo": "https://github.com/" + repository + ".git",
        "commit_sha": head,
        "steps": [{"run": shlex.join(command)}],
        "timeout": "3h",
        "wait_until": "QUEUED",
        "platform_properties": {
            "OSFamily": "linux",
            "Arch": "amd64",
            "Pool": config["pool"],
            "workload-isolation-type": "none",
            "container-image": "none",
            "EstimatedComputeUnits": "1",
        },
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("submit", "run-coordinator"))
    parser.add_argument("--config", default="/var/lib/llm-cc/comparison.json")
    parser.add_argument("--repository", required=True)
    parser.add_argument("--head")
    parser.add_argument("--branch")
    parser.add_argument("--default-branch")
    args = parser.parse_args(argv)
    repo = os.getcwd()
    if not args.head or not args.branch or not args.default_branch:
        head, branch, default_branch = resolve_checkout(
            repo, default_branch=args.default_branch
        )
        args.head = args.head or head
        args.branch = args.branch or branch
        args.default_branch = args.default_branch or default_branch
    if args.action == "run-coordinator":
        config = read_json(args.config)
        api_key_env = config.get("api_key_env", "BUILDBUDDY_API_KEY")
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", api_key_env):
            raise ValueError("invalid API credential environment variable name")
        if api_key_env != "BUILDBUDDY_API_KEY":
            # The submitting machine may use a custom variable name. Bind it
            # here to the server's runner credential, never the local API key.
            os.environ[api_key_env] = os.environ["BUILDBUDDY_API_KEY"]
        args.repo = repo
        args.output_dir = os.environ["BUILDBUDDY_ARTIFACTS_DIRECTORY"]
        args.pipeline_id = parent_invocation_id(args.output_dir)
        return coordinate(args)
    config = read_json(args.config)
    key = os.environ[config.get("api_key_env", "BUILDBUDDY_API_KEY")]
    # Setup stores a complete immutable generation alongside its profile. Avoid
    # reading the mutable comparison.json pointer after the remote job queues.
    pinned_config = Path(config["profile"]).with_name("comparison.json")
    if read_json(pinned_config) != config:
        raise ValueError(
            "configuration differs from its published immutable generation"
        )
    request = coordinator_request(
        config,
        str(pinned_config),
        args.repository,
        args.head,
        args.branch,
        args.default_branch,
    )
    invocation = BuildBuddy(config["endpoint"], key).submit(request)
    print(config["endpoint"].rstrip("/") + "/invocation/" + invocation)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
