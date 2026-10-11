"""Expose exact ratios in the checksum-verified comment for downstream policy.

This standalone entrypoint runs with Python -I beside the pinned coordinator,
including when the installed scorer predates the named lmcc_per_token fields.
"""

import hashlib
import json
import math
from pathlib import Path
import sys

PREFIX = "LM-CC/token data: `"
DATA_OPEN = "\n\n<details>\n<summary>Comparison data</summary>\n\n"
MAX_COMMENT_BYTES = 24576


def ratio(totals):
    tokens, complexity = totals.get("token_count"), totals.get("llm_cc")
    if tokens is None or complexity is None:
        return None
    if type(tokens) is not int or tokens < 0:
        raise ValueError("Invalid token count")
    if type(complexity) not in (int, float) or not math.isfinite(complexity) or complexity < 0:
        raise ValueError("Invalid LM-CC measurement")
    if tokens == 0:
        return None
    result = complexity / tokens
    if not math.isfinite(result):
        raise ValueError("Invalid LM-CC/token ratio")
    return result


def annotate(directory):
    directory = Path(directory)
    if not (directory / "report.json").exists() and (directory / "skipped.json").exists():
        return
    report = json.loads((directory / "report.json").read_text())
    publication = json.loads((directory / "publication.json").read_text())
    comment = (directory / "comment.md").read_bytes()
    if report.get("schema_version") != 2 or publication.get("schema_version") != 2:
        raise ValueError("Expected llm-cc comparison schema 2")
    if report.get("identity") != publication.get("identity") or report.get("fingerprint") != publication.get("fingerprint"):
        raise ValueError("Report and publication identities differ")
    if publication.get("status") != report.get("status"):
        raise ValueError("Report and publication statuses differ")
    expected = publication["comment"]
    if expected["path"] != "comment.md" or expected["bytes"] != len(comment) or expected["sha256"] != hashlib.sha256(comment).hexdigest():
        raise ValueError("Comment does not match its publication envelope")
    data = {"schema_version": 1, "identity": report["identity"],
            "fingerprint": report["fingerprint"], "status": report["status"],
            "base_ratio": None, "head_ratio": None}
    if report["status"] == "complete" and not report.get("errors"):
        for side in ("base", "head"):
            data[side + "_ratio"] = ratio(report["sides"][side]["totals"])
    encoded = json.dumps(data, sort_keys=True, separators=(",", ":"), allow_nan=False)
    # Publication forbids mentions and link/HTML delimiters, even in code spans.
    for char in "@[]<>`":
        encoded = encoded.replace(char, "\\u%04x" % ord(char))
    payload = (DATA_OPEN + PREFIX + encoded + "`\n\n</details>\n").encode()
    comment = comment.split(DATA_OPEN.encode(), 1)[0].rstrip()
    # Older pinned scorers can publish the same human summary before rollout.
    if comment.startswith(b"### llm-cc"):
        title, separator, rest = comment.partition(b"\n")
        if b"LM-CC/token" not in title:
            base, head = data["base_ratio"], data["head_ratio"]
            display = lambda value: "unavailable" if value is None else format(value, ".6g")
            title += (" · LM-CC/token " + display(base) + " → " + display(head)).encode()
            if base is not None and head is not None and base != 0:
                title += (" (%+.3g%%)" % ((head - base) / base * 100)).encode()
            comment = title + separator + rest
    body = comment + payload
    if len(body) > MAX_COMMENT_BYTES:
        lead = comment.split(b"\n\n<details>", 1)[0]
        body = lead + b"\n\nFull comparison details are available in report artifacts.\n" + payload
    if len(body) > MAX_COMMENT_BYTES:
        raise ValueError("Ratio identity and summary exceed the comment limit")
    publication["comment"] = {"path": "comment.md", "bytes": len(body),
                              "sha256": hashlib.sha256(body).hexdigest()}
    (directory / "comment.md").write_bytes(body)
    (directory / "publication.json").write_text(
        json.dumps(publication, sort_keys=True, separators=(",", ":")) + "\n")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: ratchet.py COMPARISON_ARTIFACT_DIRECTORY")
    annotate(sys.argv[1])
