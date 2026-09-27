"""Offline fixtures shared by the comparison tests.

Scoring arguments that the deterministic llm-cc test build accepts, Git
repository helpers, and an in-memory GitHub behind the real REST client. The
name avoids the test_ prefix, so discovery never collects this module as tests.
"""

from __future__ import annotations

import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
import urllib.error
import urllib.parse

from .common import llm_cc, read_json
from .github import GitHub

SCORER_IMAGE = "registry.example/llm-cc-scorer@sha256:" + "1" * 64


def git(repo, *args):
    env = os.environ | {
        "GIT_AUTHOR_NAME": "Test",
        "GIT_AUTHOR_EMAIL": "test@example.test",
        "GIT_COMMITTER_NAME": "Test",
        "GIT_COMMITTER_EMAIL": "test@example.test",
    }
    return (
        subprocess.check_output(["git", "-C", str(repo), *args], env=env)
        .decode()
        .strip()
    )


def commit_files(repo, files, message):
    """Write (or with None, delete) files, commit them, and return the SHA."""
    for name, content in files.items():
        path = Path(repo) / name
        if content is None:
            git(repo, "rm", "-q", name)
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content if isinstance(content, bytes) else content.encode())
    git(repo, "add", "-A")
    git(repo, "commit", "-qm", message)
    return git(repo, "rev-parse", "HEAD")


def fake_scoring(root):
    """A model and scoring.args for llm-cc-compare-fake; returns (model, args).

    The test build hashes any file as its model and scores deterministically,
    so plans, workers and reports run end to end without inference.
    """
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    model = root / "model.gguf"
    model.write_bytes(b"model")
    arguments = root / "scoring.args"
    arguments.write_text(
        "# The deterministic test scorer's explicit contract.\n"
        "--model\n%s\n--backend\ncpu\n--entropy-reduction\nhost\n" % model
    )
    return model, arguments


def effective_rules(rules):
    """`rules` as `llm-cc compare prepare` loads them for a target commit
    without its own: {rules, rules_source} from a one-file plan."""
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        repo = root / "repo"
        repo.mkdir()
        git(repo, "init", "-q")
        head = commit_files(repo, {"main.rs": "fn main() {}\n"}, "only")
        model, scoring = fake_scoring(root / "scoring")
        (root / "identity.json").write_text(
            json.dumps({"repository": "o/r", "pipeline_id": "p"})
        )
        subprocess.run(
            [
                llm_cc(),
                "compare",
                "prepare",
                "--repo",
                str(repo),
                "--head",
                head,
                "--target",
                head,
                "--identity",
                str(root / "identity.json"),
                "--output-dir",
                str(root / "out"),
                "--default-rules",
                str(rules),
                "@" + str(scoring),
            ],
            check=True,
            capture_output=True,
        )
        plan = read_json(root / "out/plan.json")
    return {"rules": plan["rules"], "rules_source": plan["rules_source"]}


class FakeGitHub(GitHub):
    """An in-memory GitHub REST API served behind the real client's `call`.

    Routing below the client exercises its URL construction and pagination.
    `lose_next_write` applies a write and then fails as a lost response would.
    """

    def __init__(self, repository="owner/repo", login="llm-cc-bot"):
        super().__init__(repository, "synthetic-token")
        self.login = login
        self.branches = {}
        self.pulls = {}
        self.comments = {}
        self.next_comment_id = 1000
        self.calls = []
        self.lose_next_write = False

    def open_pull(
        self,
        number,
        head_ref,
        head_sha,
        base_ref="main",
        base_repo=None,
        head_repo=None,
    ):
        self.pulls[number] = {
            "number": number,
            "state": "open",
            "head": {
                "ref": head_ref,
                "sha": head_sha,
                "repo": {"full_name": head_repo or self.repository},
            },
            "base": {
                "ref": base_ref,
                "sha": self.branches[base_ref],
                "repo": {"full_name": base_repo or self.repository},
            },
        }
        return self.pulls[number]

    def add_comment(self, number, body, login):
        self.next_comment_id += 1
        comment = {
            "id": self.next_comment_id,
            "issue": number,
            "user": {"login": login},
            "body": body,
        }
        self.comments[comment["id"]] = comment
        return comment

    def pull_comments(self, number):
        return [
            comment
            for _, comment in sorted(self.comments.items())
            if comment["issue"] == number
        ]

    @staticmethod
    def _page(values, query):
        size = int(query.get("per_page", ["30"])[0])
        page = int(query.get("page", ["1"])[0])
        return copy.deepcopy(values[(page - 1) * size : page * size])

    def _missing(self, path):
        return urllib.error.HTTPError(self.api_url + path, 404, "Not Found", {}, None)

    def _written(self, value):
        if self.lose_next_write:
            self.lose_next_write = False
            raise urllib.error.URLError("connection reset after the write")
        return copy.deepcopy(value)

    def call(self, method, path, payload=None):
        self.calls.append((method, path))
        parts = urllib.parse.urlsplit(path)
        query = urllib.parse.parse_qs(parts.query)
        if method == "GET" and parts.path == "/user":
            return {"login": self.login}
        prefix = "/repos/" + self.repository + "/"
        if not parts.path.startswith(prefix):
            raise self._missing(path)
        route = parts.path[len(prefix) :].split("/")
        if method == "GET" and route[0] == "commits" and len(route) == 3:
            pulls = [
                pull
                for _, pull in sorted(self.pulls.items())
                if pull["head"]["sha"] == route[1]
            ]
            return self._page(pulls, query)
        if method == "GET" and route[0] == "commits" and len(route) == 2:
            branch = urllib.parse.unquote(route[1])
            if branch not in self.branches:
                raise self._missing(path)
            return {"sha": self.branches[branch]}
        if method == "GET" and route[0] == "pulls" and len(route) == 2:
            if int(route[1]) not in self.pulls:
                raise self._missing(path)
            return copy.deepcopy(self.pulls[int(route[1])])
        if route[0] == "issues" and route[1] == "comments" and method == "PATCH":
            comment = self.comments.get(int(route[2]))
            if comment is None:
                raise self._missing(path)
            comment["body"] = payload["body"]
            return self._written(comment)
        if route[0] == "issues" and route[2:] == ["comments"]:
            number = int(route[1])
            if method == "GET":
                return self._page(self.pull_comments(number), query)
            if method == "POST":
                return self._written(
                    self.add_comment(number, payload["body"], self.login)
                )
        raise AssertionError("unexpected GitHub call %s %s" % (method, path))
