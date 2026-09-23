"""Offline fixtures shared by the comparison tests.

A synthetic scorer and profile that the real worker accepts, Git repository
helpers, and an in-memory GitHub behind the real REST client. The name avoids
the test_ prefix, so discovery never collects this module as tests.
"""

from __future__ import annotations

import copy
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import urllib.error
import urllib.parse

from .common import CONTAINER_ENVIRONMENT_POLICY
from .github import GitHub

SCORER_IMAGE = "registry.example/llm-cc-scorer@sha256:" + "1" * 64

# Emits the complete JSONL contract the worker validates: configuration, one
# result per input with a deterministic score, and consistent totals. The
# worker runs scorers with a sanitized PATH, so name the interpreter exactly.
SCORER = (
    "#!"
    + sys.executable
    + """
import hashlib,json,sys
language=sys.argv[sys.argv.index('--lang')+1]
model=sys.argv[sys.argv.index('--model')+1]
paths=sys.argv[sys.argv.index('--include-headers')+1:]
print(json.dumps({'type':'configuration','model_sha256':hashlib.sha256(open(model,'rb').read()).hexdigest(),'model_size':len(open(model,'rb').read()),'language':language,'no_download':True,'no_ignore':True,'include_headers':True}))
score=tokens=0
for path in paths:
 data=open(path,'rb').read(); value=float(sum(data)); count=len(data); score+=value; tokens+=count
 print(json.dumps({'type':'file','path':path,'language':language,'llm_cc':value,'token_count':count}))
print(json.dumps({'type':'totals','discovered':len(paths),'analyzed':len(paths),'failed':0,'partial':False,'llm_cc':score,'token_count':tokens}))
"""
)


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


def synthetic_scorer(root, execution_image=SCORER_IMAGE, max_file_bytes=65536):
    """Install the synthetic scorer and model; returns (scorer, model, profile)."""
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    scorer = root / "fake-scorer.py"
    scorer.write_text(SCORER)
    scorer.chmod(0o755)
    model = root / "model.gguf"
    model.write_bytes(b"model")
    profile = {
        "scoring": {"expected_configuration": {}},
        "build": {
            "installed_files": {
                scorer.name: hashlib.sha256(scorer.read_bytes()).hexdigest()
            },
            "source_commit": "4646123",
            "inference_abi": "test",
            "execution_image": execution_image,
            "container_environment_policy": CONTAINER_ENVIRONMENT_POLICY,
            "model_sha256": hashlib.sha256(b"model").hexdigest(),
            "model_bytes": 5,
        },
        "max_file_bytes": max_file_bytes,
    }
    return scorer, model, profile


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
