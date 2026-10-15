"""Resolve the comparison identity for one push before any scoring work.

Default-branch pushes compare the head with itself and seed the baseline cache.
Any other branch resolves its single current open pull request and that PR's
actual target; a branch without one is skipped. Discovery only reads GitHub.

Without a token, the REST API allows 60 reads an hour per source IP, shared
with everything else on the host. Anonymous coordinators therefore read the
same facts through GitHub's Git refs, which draw on no REST quota, and ask the
API only when those refs cannot answer.
"""

from __future__ import annotations

from datetime import datetime, timezone
import json
from pathlib import Path
import re
import subprocess
import sys
import time

from .common import write_json

SKIP_VARIABLE = "COMPARISON_SKIP"
# GitHub computes a pull request's test merge asynchronously after a push.
GIT_MERGE_DELAY_SECONDS = 5
# A stalled Git endpoint must not hold the coordinator past its deadline.
GIT_TIMEOUT_SECONDS = 60
# Waiting for GitHub's test merge, after which discovery asks the REST API.
GIT_DISCOVERY_SECONDS = 90
GIT_ERRORS = (subprocess.CalledProcessError, subprocess.TimeoutExpired)


class Unresolved(Exception):
    """Git refs alone cannot establish the identity; the REST API must."""


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


def _git_output(repo, *args):
    return subprocess.run(
        ["git", "-C", str(repo), *args],
        check=True,
        stdout=subprocess.PIPE,
        text=True,
        timeout=GIT_TIMEOUT_SECONDS,
    ).stdout


def branch_ref(name):
    """refs/heads/NAME for a valid branch name; never an option or pattern."""
    ref = "refs/heads/" + name
    if subprocess.run(["git", "check-ref-format", ref]).returncode != 0:
        raise ValueError("invalid branch name: %r" % name)
    return ref


def ls_remote(repo, refs, remote="origin"):
    """The named refs' current values on `remote`, read over the Git protocol."""
    wanted = set(refs)
    values = {}
    for line in _git_output(repo, "ls-remote", remote, *refs).splitlines():
        sha, _, name = line.partition("\t")
        if name in wanted:
            values[name] = sha
    return values


def merge_parents(repo, ref, remote="origin"):
    """Fetch GitHub's test merge `ref`; returns its SHA and parent SHAs.

    A refused fetch returns no parents, so the caller waits for GitHub; a
    stalled one cannot be waited out and leaves discovery to the API.
    """
    try:
        subprocess.run(
            ["git", "-C", str(repo), "fetch", "--quiet", "--no-tags", remote, ref],
            check=True,
            timeout=GIT_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired:
        raise Unresolved("fetching %s timed out" % ref) from None
    except subprocess.CalledProcessError:
        return None, []
    merge = _git_output(repo, "rev-parse", "FETCH_HEAD").strip()
    return merge, _git_output(repo, "show", "-s", "--format=%P", merge).split()


def _advertised(repo, refs, remote):
    try:
        return ls_remote(repo, refs, remote)
    except GIT_ERRORS as error:
        raise Unresolved("cannot read %s's refs: %s" % (remote, error)) from None


def push_from_git(repo, head, branch, remote="origin"):
    """A default-branch push's identity, or ValueError once it is superseded."""
    ref = branch_ref(branch)
    if _advertised(repo, [ref], remote).get(ref) != head:
        raise ValueError("default-branch push was superseded")
    return identity_of(head, head, branch, None)


def identity_of(head, target_sha, target_branch, pr_number):
    """The identity fields discovery resolves."""
    return {
        "head_sha": head,
        "target_sha": target_sha,
        "target_branch": target_branch,
        "pr_number": pr_number,
    }


def pull_from_git(
    repo,
    head,
    branch,
    pr_number,
    target_branch,
    remote="origin",
    delay=GIT_MERGE_DELAY_SECONDS,
    sleep=time.sleep,
    deadline_seconds=GIT_DISCOVERY_SECONDS,
    clock=time.monotonic,
):
    """PR `pr_number`'s identity as GitHub records it, read from its Git refs.

    GitHub's test merge, refs/pull/N/merge, has the PR's recorded base as its
    first parent and its head as its second: the same base.sha the REST API
    reports, which is what consumers compare against. Returns (identity, merge
    SHA), or (None, None) when this repository's branch no longer proposes
    `head`, as REST discovery skips such a push. Raises Unresolved while GitHub
    has no merge for this head; it never computes one for a conflicting PR.
    Waits end at `deadline_seconds`, and a stalled command ends them at once,
    so discovery never holds the run for more than about two minutes.
    """
    branch_ref(target_branch)
    refs = [
        branch_ref(branch),
        "refs/pull/%d/head" % pr_number,
        "refs/pull/%d/merge" % pr_number,
    ]
    deadline = clock() + deadline_seconds
    while True:
        proposed, pull_head, merge = map(_advertised(repo, refs, remote).get, refs)
        # Like REST discovery, require this repository's own branch to propose
        # the head: a fork's same-named branch never selects the comparison.
        if proposed != head:
            return None, None
        # The branch moved here first; GitHub updates the PR's refs after it.
        if pull_head == head and merge:
            merge, parents = merge_parents(repo, refs[2], remote)
            if parents[1:] == [head]:
                return identity_of(head, parents[0], target_branch, pr_number), merge
        if clock() + delay > deadline:
            raise Unresolved(
                "GitHub has no test merge of PR #%d at %s" % (pr_number, head[:12])
            )
        sleep(delay)


def discover_anonymous(
    github,
    identity,
    repo,
    branch,
    default_branch,
    pr_number=None,
    target_branch=None,
    **retry,
):
    """discover_identity that spends REST quota only when Git refs cannot answer.

    `pr_number` and `target_branch` come from the CI event, which names the
    pull request a push belongs to. Returns (identity or None, merge SHA or None).
    """
    head = identity["head_sha"]
    try:
        resolved, merge = from_git(
            repo, head, branch, default_branch, pr_number, target_branch, retry
        )
    except Unresolved as reason:
        print("%s; asking the GitHub API" % reason, file=sys.stderr, flush=True)
        resolved, merge = from_rest(github, head, branch, default_branch, pr_number)
    return (None if resolved is None else dict(identity, **resolved)), merge


def from_git(repo, head, branch, default_branch, pr_number, target_branch, retry):
    """Git-ref discovery: (identity or None, merge SHA or None), or Unresolved."""
    if branch == default_branch:
        return push_from_git(repo, head, branch), None
    if pr_number and target_branch:
        return pull_from_git(repo, head, branch, pr_number, target_branch, **retry)
    raise Unresolved("the CI event names no pull request for " + branch)


def from_rest(github, head, branch, default_branch, pr_number):
    """REST discovery, which knows no merge: one read of a named PR, else a search."""
    if pr_number:
        return github.discover_pull(pr_number, head, branch), None
    return github.discover(head, branch, default_branch), None


def current_from_git(repo, identity, merge=None, remote="origin"):
    """Freshness from Git refs: True or False, or None when only the API can tell.

    A newer push moves the branch or the PR's head ref. While the test merge
    is still the one discovery read, GitHub has recorded no other base for
    this head. Any other merge state (recomputed, as on retargeting, or
    dropped, as on close) and an unreachable endpoint defer to the API.
    """
    head = identity["head_sha"]
    try:
        if identity["pr_number"] is None:
            ref = branch_ref(identity["target_branch"])
            return ls_remote(repo, [ref], remote).get(ref) == head
        pull_head = "refs/pull/%d/head" % identity["pr_number"]
        merge_ref = "refs/pull/%d/merge" % identity["pr_number"]
        refs = ls_remote(repo, [pull_head, merge_ref], remote)
    except GIT_ERRORS:
        return None
    if refs.get(pull_head) != head:
        return False
    # The head ref alone says nothing about the PR's state, base or target.
    if merge is None or refs.get(merge_ref) != merge:
        return None
    return True


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
