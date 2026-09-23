"""GitHub REST client shared by discovery, the coordinators and the publisher.

Discovery needs only read access. The publisher alone writes, and only issue
comments; credentials remain environment inputs, never artifacts.
"""

from __future__ import annotations

import json
import re
import urllib.parse
import urllib.request

from .common import canonical_bytes

API_URL = "https://api.github.com"


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
    def __init__(self, repository, token="", api_url=API_URL):
        if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
            raise ValueError("repository must be owner/name")
        if not api_url.startswith("https://"):
            raise ValueError("GitHub API URL must use HTTPS")
        self.repository, self.token = repository, token
        self.api_url = api_url.rstrip("/")

    def call(self, method, path, payload=None):
        return api_request(self.api_url + path, self.token, payload, method=method)

    def get(self, suffix):
        return self.call("GET", "/repos/" + self.repository + suffix)

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
            pulls.extend(
                p
                for p in batch
                if p["state"] == "open"
                and p["head"]["sha"] == head
                and p["head"]["ref"] == branch
                and p["base"]["repo"]["full_name"] == self.repository
                # A fork can propose the same commit under the same branch
                # name; a deleted fork has no head repository at all.
                and (p["head"]["repo"] or {}).get("full_name") == self.repository
            )
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
        return {
            "head_sha": head,
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
