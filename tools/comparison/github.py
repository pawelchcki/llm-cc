"""GitHub REST client shared by discovery, the coordinators and the publisher.

Discovery needs only read access. The publisher alone writes, and only issue
comments; credentials remain environment inputs, never artifacts.
"""

from __future__ import annotations

import json
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

from .common import canonical_bytes

API_URL = "https://api.github.com"
# GitHub's documented fallback for a secondary limit that names no wait.
SECONDARY_LIMIT_SECONDS = 60


class RateLimited(RuntimeError):
    """GitHub refused a request for quota; retrying after `retry_at` can succeed."""

    def __init__(self, path, retry_at):
        self.retry_at = retry_at
        super().__init__(
            "GitHub API rate limit for %s; retry after %s"
            % (
                path,
                time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(retry_at)),
            )
        )


def _named_wait(error, now):
    """The wait a limit's headers name: retry-after, or a spent window's reset."""
    headers = error.headers or {}
    retry_after = headers.get("retry-after")
    if retry_after is not None and retry_after.strip().isdigit():
        return max(1, int(retry_after))
    reset = headers.get("x-ratelimit-reset")
    if headers.get("x-ratelimit-remaining") == "0" and reset and reset.isdigit():
        # One extra second absorbs clock skew at the window boundary.
        return max(1, int(reset) - int(now) + 1)
    return None


def rate_limit_delay(error, now=None, unnamed=0):
    """Seconds GitHub asks a client to wait, or None if `error` is not a limit.

    Primary limits answer 403 or 429 with x-ratelimit-remaining: 0 and the
    window's reset time. Secondary limits send retry-after, or only a message;
    GitHub then asks for a minute, doubled for each of the `unnamed`
    consecutive limits before it that named no wait either.
    """
    if error.code not in (403, 429):
        return None
    named = _named_wait(error, time.time() if now is None else now)
    if named is not None:
        return named
    # GitHub's status line already reads "rate limit exceeded" for a primary
    # limit; a secondary limit without retry-after says so in its body.
    message = str(getattr(error, "reason", "") or "").lower()
    if "rate limit" not in message:
        try:
            message = error.read(4096).decode("utf-8", "replace").lower()
        except Exception:
            message = ""
    if "rate limit" not in message:
        return None
    return SECONDARY_LIMIT_SECONDS * 2 ** min(unnamed, 10)


def api_request(
    url,
    token,
    payload=None,
    token_header="Authorization",
    content_type="application/json",
    method=None,
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
    request = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(request, timeout=45) as response:
        body = response.read()
    if content_type != "application/json":
        return body
    return json.loads(body) if body else None


class GitHub:
    def __init__(self, repository, token="", api_url=API_URL, rate_limit_wait=0):
        if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
            raise ValueError("repository must be owner/name")
        if not api_url.startswith("https://"):
            raise ValueError("GitHub API URL must use HTTPS")
        self.repository, self.token = repository, token
        self.api_url = api_url.rstrip("/")
        # Total seconds this client may sleep for quota; beyond it, RateLimited.
        self.rate_limit_wait = rate_limit_wait
        self.sleep = time.sleep

    def call(self, method, path, payload=None):
        unnamed = 0
        while True:
            try:
                return api_request(
                    self.api_url + path, self.token, payload, method=method
                )
            except urllib.error.HTTPError as error:
                unnamed = self._wait(error, path, unnamed)

    def _wait(self, error, path, unnamed):
        """Sleep out a quota refusal within the budget; re-raise any other error.

        Returns how many consecutive limits have now named no wait.
        """
        delay = rate_limit_delay(error, unnamed=unnamed)
        if delay is None:
            raise error
        # A limited request was not processed, so a write retries safely.
        error.close()
        if delay > self.rate_limit_wait:
            raise RateLimited(path, time.time() + delay) from None
        print(
            "GitHub API rate limit for %s; retrying in %d seconds" % (path, delay),
            file=sys.stderr,
            flush=True,
        )
        self.rate_limit_wait -= delay
        self.sleep(delay)
        return 0 if _named_wait(error, time.time()) is not None else unnamed + 1

    def get(self, suffix):
        return self.call("GET", "/repos/" + self.repository + suffix)

    def core_remaining(self):
        """Unused REST quota, or None if unknown. Reading it costs no quota."""
        try:
            limits = api_request(self.api_url + "/rate_limit", self.token)
            return int(limits["resources"]["core"]["remaining"])
        except Exception:
            return None

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
            pulls.extend(p for p in batch if self._proposes(p, head, branch))
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
        return self._pull_identity(pull)

    def discover_pull(self, number, head, branch):
        """discover() for the PR a CI event names: one read, no search."""
        pull = self.get(f"/pulls/{int(number)}")
        return self._pull_identity(pull) if self._proposes(pull, head, branch) else None

    def _proposes(self, pull, head, branch):
        # GitHub repository names are case-insensitive, and a configured
        # repository may differ from the canonical full_name in case.
        repository = self.repository.lower()
        return (
            pull["state"] == "open"
            and pull["head"]["sha"] == head
            and pull["head"]["ref"] == branch
            and pull["base"]["repo"]["full_name"].lower() == repository
            # A fork can propose the same commit under the same branch
            # name; a deleted fork has no head repository at all.
            and ((pull["head"]["repo"] or {}).get("full_name") or "").lower()
            == repository
        )

    @staticmethod
    def _pull_identity(pull):
        return {
            "head_sha": pull["head"]["sha"],
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

    def issue_comments(self, number):
        comments = []
        page = 1
        while True:
            batch = self.get(f"/issues/{int(number)}/comments?per_page=100&page={page}")
            comments.extend(batch)
            if len(batch) < 100:
                return comments
            page += 1

    def create_comment(self, number, body):
        return self.call(
            "POST",
            f"/repos/{self.repository}/issues/{int(number)}/comments",
            {"body": body},
        )

    def update_comment(self, comment_id, body):
        return self.call(
            "PATCH",
            f"/repos/{self.repository}/issues/comments/{int(comment_id)}",
            {"body": body},
        )

    def authenticated_login(self):
        # Installation tokens, such as Actions' GITHUB_TOKEN, cannot read
        # /user; their callers must name the comment author explicitly.
        return self.call("GET", "/user")["login"]
