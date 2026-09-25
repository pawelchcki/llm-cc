"""Store pipeline reports and publish one ordered pull-request comment.

`store-report` places a finished report under its pipeline's store prefix.
`publish` validates that report against trusted CI inputs, orders publication
per pull request with a conditional-write marker, re-reads the pull request,
and upserts the single comment its own account wrote. Scores are advisory:
stale and suppressed publications succeed with a printed reason, while
validation, storage and API failures do not.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import tempfile
import urllib.error

from .cache import CacheError
from .common import (
    aggregate_report,
    canonical_bytes,
    digest,
    pipeline_prefix,
    read_json,
)
from .pipeline import COMMENT_LIMIT

# Stored in this order: the publication envelope comes last, because its
# presence is what tells `publish` that the report is complete. Storing first
# replaces any envelope with INCOMPLETE, so a retry that fails part-way under
# the same pipeline ID never leaves an earlier attempt's report looking current.
REPORT_FILES = (
    "report.json",
    "report.md",
    "comment.md",
    "baseline.md",
    "baseline.json",
    "publication.json",
)
INCOMPLETE = b'{"incomplete":true}\n'
COMMENT_MARKER = "<!-- llm-cc-comparison -->"
_ORDER = re.compile(r"<!-- llm-cc-comparison pipeline=(\S+) ordinal=(\d+) -->")
PIPELINE_ID = re.compile(r"[A-Za-z0-9._:-]{1,128}")
MARKER_STATES = ("reserved", "published", "suppressed")
# Conflicts only arise from concurrent publishers for one pull request, which
# per-branch CI serialization already prevents; a few retries are plenty.
ATTEMPTS = 8


class PublicationError(ValueError):
    """A report, identity or marker failed validation."""


def _check_comment(publication, comment):
    entry = publication.get("comment") if isinstance(publication, dict) else None
    if (
        not isinstance(entry, dict)
        or entry.get("path") != "comment.md"
        or entry.get("bytes") != len(comment)
        or entry.get("sha256") != hashlib.sha256(comment).hexdigest()
        or len(comment) > COMMENT_LIMIT
    ):
        raise PublicationError("comment.md does not match its publication envelope")


def _check_identity(identity, expected):
    if not isinstance(identity, dict) or not isinstance(expected, dict):
        raise PublicationError("report has no identity")
    for name, value in expected.items():
        if identity.get(name) != value:
            raise PublicationError(
                "report %s %r does not match the pipeline's %r"
                % (name, identity.get(name), value)
            )


def store_report(store, report_dir, identity=None):
    """Upload a report directory under the prefix of the pipeline it describes.

    A child pipeline's aggregate job stores under the parent pipeline ID
    carried in the plan identity, which is what the parent's publisher reads.
    """
    directory = Path(report_dir)
    publication = read_json(directory / "publication.json")
    _check_comment(publication, (directory / "comment.md").read_bytes())
    report_identity = publication.get("identity")
    _check_identity(
        read_json(directory / "report.json").get("identity"), report_identity
    )
    if not all(
        isinstance(report_identity.get(name), str)
        for name in ("repository", "pipeline_id")
    ):
        raise PublicationError("report does not name its repository and pipeline")
    if identity is not None:
        _check_identity(
            report_identity,
            {name: identity[name] for name in ("repository", "pipeline_id")},
        )
    prefix = pipeline_prefix(identity or report_identity)
    store.put(prefix + "publication.json", INCOMPLETE)
    for name in REPORT_FILES:
        store.put(prefix + name, (directory / name).read_bytes())
    return prefix


def load_publication(store, prefix):
    """The stored envelope and comment, or None when absent or incomplete."""
    raw = store.get(prefix + "publication.json")
    if raw is None or raw == INCOMPLETE:
        return None
    try:
        publication = json.loads(raw)
    except (UnicodeDecodeError, ValueError) as error:
        raise PublicationError("stored publication.json is not JSON") from error
    comment = store.get(prefix + "comment.md")
    if comment is None:
        raise PublicationError("stored publication has no comment.md")
    _check_comment(publication, comment)
    return publication, comment


def marker_key(repository, pr_number):
    # One comment belongs to one pull request, whichever branch updates it.
    return "publications/" + digest([repository, pr_number]) + ".json"


def _read_marker(store, key):
    raw, token = store.get_versioned(key)
    if raw is None:
        return None, None
    try:
        marker = json.loads(raw)
        valid = (
            marker["schema_version"] == 1
            and type(marker["ordinal"]) is int
            and marker["ordinal"] >= 0
            and isinstance(marker["pipeline_id"], str)
            and isinstance(marker["head_sha"], str)
            and marker["state"] in MARKER_STATES
            and (marker["comment_id"] is None or type(marker["comment_id"]) is int)
        )
    except (UnicodeDecodeError, ValueError, KeyError, TypeError):
        valid = False
    if not valid:
        # Overwriting an unreadable marker could reorder publication; an
        # operator must inspect and remove it instead.
        raise PublicationError("publication marker %s is malformed" % key)
    return marker, token


def _reserve(store, key, record):
    """Claim the marker for `record`, or return the newer marker that wins."""
    for _ in range(ATTEMPTS):
        marker, token = _read_marker(store, key)
        if marker is not None:
            if marker["ordinal"] > record["ordinal"]:
                return None, marker
            if (
                marker["ordinal"] == record["ordinal"]
                and marker["pipeline_id"] != record["pipeline_id"]
            ):
                raise PublicationError(
                    "pipelines %s and %s share publication ordinal %d"
                    % (marker["pipeline_id"], record["pipeline_id"], record["ordinal"])
                )
            record = dict(record, comment_id=marker["comment_id"])
        written = store.put_if(key, canonical_bytes(record), token)
        if written is not None:
            return (record, written), None
    raise CacheError("publication marker %s kept changing" % key)


def _finish(store, key, token, record):
    """Record completion unless a newer pipeline has reserved since."""
    for _ in range(ATTEMPTS):
        if store.put_if(key, canonical_bytes(record), token) is not None:
            return True
        marker, token = _read_marker(store, key)
        if marker is not None and marker["ordinal"] > record["ordinal"]:
            return False
    raise CacheError("publication marker %s kept changing" % key)


def _comment_ordinal(body):
    # Only the header line counts; the rendered report below it is untrusted.
    lines = body.split("\n", 2)
    match = _ORDER.fullmatch(lines[1].rstrip("\r")) if len(lines) > 1 else None
    return int(match.group(2)) if match else -1


def comment_body(comment, pipeline_id, ordinal):
    return "%s\n<!-- llm-cc-comparison pipeline=%s ordinal=%d -->\n%s" % (
        COMMENT_MARKER,
        pipeline_id,
        ordinal,
        comment.decode("utf-8"),
    )


def _own_comment(comments, author, preferred_id):
    """The comment this publisher owns; other authors' copies of the marker are ignored."""
    # GitHub logins are case-insensitive; --comment-author may differ in case.
    author = author.lower()
    ours = [
        comment
        for comment in comments
        if ((comment.get("user") or {}).get("login") or "").lower() == author
        and isinstance(comment.get("body"), str)
        # Editing a comment in the web UI switches it to CRLF line endings.
        and comment["body"].split("\n", 1)[0].rstrip("\r") == COMMENT_MARKER
    ]
    for comment in ours:
        if comment["id"] == preferred_id:
            return comment
    return min(ours, key=lambda comment: comment["id"], default=None)


def _failure(store, prefix, identity, reason):
    with tempfile.TemporaryDirectory(prefix="llm-cc-publish-") as directory:
        aggregate_report(directory, identity=identity, errors=[reason])
        store_report(store, directory)
    return load_publication(store, prefix)


def publish(
    store,
    github,
    *,
    repository,
    pipeline_id,
    head,
    ordinal,
    identity=None,
    failure=None,
    comment_author=None,
):
    """Publish one pipeline's report; returns (outcome, reason)."""
    if not PIPELINE_ID.fullmatch(pipeline_id):
        raise PublicationError("pipeline ID must match " + PIPELINE_ID.pattern)
    if type(ordinal) is not int or ordinal < 0:
        raise PublicationError("ordinal must be a non-negative integer")
    trusted = {"repository": repository, "pipeline_id": pipeline_id, "head_sha": head}
    if identity is not None:
        _check_identity(identity, trusted)
    prefix = pipeline_prefix(trusted)
    loaded = None if failure else load_publication(store, prefix)
    if loaded is None:
        # Failed image, preparation or scoring stages leave no report; the
        # discovery identity still lets the pull request learn about it.
        if identity is None:
            raise PublicationError(
                "a failure publication needs --identity"
                if failure
                else "no stored report for pipeline %s and no --identity to "
                "report the failure against" % pipeline_id
            )
        reason = (
            failure or "no comparison report was stored for pipeline " + pipeline_id
        )
        loaded = _failure(store, prefix, identity, reason)
    publication, comment = loaded
    report_identity = publication.get("identity")
    _check_identity(report_identity, trusted)
    if identity is not None:
        _check_identity(
            report_identity,
            {
                name: identity.get(name)
                for name in ("target_sha", "target_branch", "pr_number")
            },
        )
    pr_number = report_identity.get("pr_number")
    if pr_number is None:
        return "stored", "default-branch report stored; there is no pull request"
    if type(pr_number) is not int:
        raise PublicationError("report pr_number must be an integer or null")
    key = marker_key(repository, pr_number)
    record = {
        "schema_version": 1,
        "ordinal": ordinal,
        "pipeline_id": pipeline_id,
        "head_sha": head,
        "state": "reserved",
        "comment_id": None,
    }
    # Reserve before touching the comment: if the update's response is lost,
    # the reservation still stops an older pipeline from overwriting it.
    claimed, newer = _reserve(store, key, record)
    if claimed is None:
        return "stale", "pipeline %s (ordinal %d) already %s" % (
            newer["pipeline_id"],
            newer["ordinal"],
            newer["state"],
        )
    record, token = claimed
    if not github.current(report_identity):
        _finish(store, key, token, dict(record, state="suppressed"))
        return "suppressed", "PR #%d closed, retargeted or has a newer head" % pr_number
    if comment_author is None:
        try:
            comment_author = github.authenticated_login()
        except urllib.error.HTTPError as error:
            code = error.code
            error.close()
            raise PublicationError(
                "cannot read the token's login (HTTP %d); installation tokens "
                "must pass --comment-author" % code
            ) from None
    existing = _own_comment(
        github.issue_comments(pr_number), comment_author, record["comment_id"]
    )
    if existing is not None and _comment_ordinal(existing["body"]) > ordinal:
        _finish(store, key, token, dict(record, state="suppressed"))
        return "stale", "comment %d already shows a newer pipeline" % existing["id"]
    # A publisher that CI failed to serialize may have reserved meanwhile. This
    # narrows the window before the comment update; only serialization closes it.
    if store.get_versioned(key)[1] != token:
        marker, _ = _read_marker(store, key)
        if marker is not None and marker["ordinal"] > ordinal:
            return "stale", "pipeline %s reserved publication first" % marker[
                "pipeline_id"
            ]
        raise PublicationError("publication marker changed during publication")
    body = comment_body(comment, pipeline_id, ordinal)
    if existing is None:
        comment_id = github.create_comment(pr_number, body)["id"]
    else:
        comment_id = github.update_comment(existing["id"], body)["id"]
    if not _finish(
        store, key, token, dict(record, state="published", comment_id=comment_id)
    ):
        return "published", "comment %d updated; a newer pipeline follows" % comment_id
    return "published", "comment %d on PR #%d" % (comment_id, pr_number)
