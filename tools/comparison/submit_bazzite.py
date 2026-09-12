"""Submit a CPU coordinator using the installed, checksum-pinned host package."""

import argparse
import os
from pathlib import Path
import re
import shlex

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


def coordinator_request(config, config_path, repository, head, branch):
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
        ],
        config["execution_bundle"],
        config["execution_bundle_sha256"],
    )
    overrides = []
    for name in dict.fromkeys(
        [
            config.get("api_key_env", "BUILDBUDDY_API_KEY"),
            config.get("github_token_env", "GITHUB_TOKEN"),
        ]
    ):
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            raise ValueError("invalid credential environment variable name")
        value = os.environ.get(name)
        if value:
            if any(character in value for character in ",\n\r"):
                raise ValueError(
                    "credential environment values cannot contain commas or newlines"
                )
            overrides.append(name + "=" + value)
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
        "remote_headers": [
            "x-buildbuddy-platform.secret-env-overrides=" + ",".join(overrides)
        ]
        if overrides
        else [],
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("submit", "run-coordinator"))
    parser.add_argument("--config", default="/var/lib/llm-cc/comparison.json")
    parser.add_argument("--repository", default="pawelchcki/llm-cc")
    parser.add_argument("--head", required=True)
    parser.add_argument("--branch", required=True)
    args = parser.parse_args(argv)
    if args.action == "run-coordinator":
        args.repo = os.getcwd()
        args.default_branch = "main"
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
        config, str(pinned_config), args.repository, args.head, args.branch
    )
    invocation = BuildBuddy(config["endpoint"], key).submit(request)
    print(config["endpoint"].rstrip("/") + "/invocation/" + invocation)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
