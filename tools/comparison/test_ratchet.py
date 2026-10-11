"""Exact-ratio handoff, publication integrity, bounds and import isolation."""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from . import ratchet
from .publish import _check_comment


class RatchetTest(unittest.TestCase):
    def artifacts(self, directory, base=(10, 100), head=(10, 100), **changes):
        comment = changes.pop("comment", b"### llm-cc: repository LM-CC delta\n\nruntime +1.0\n")
        identity = {"repository": "pawelchcki/llm-cc", "head_sha": "a" * 40,
                    "target_sha": "b" * 40, "base_sha": "b" * 40, "pr_number": 1,
                    "target_branch": "release@`v1", "pipeline_id": "example",
                    "started_at": "2026-10-11T00:00:00Z"}
        report = {"schema_version": 2, "identity": identity, "fingerprint": "c" * 64,
                  "status": "complete", "errors": [], "sides": {
                      "base": {"totals": {"llm_cc": base[0], "token_count": base[1]}},
                      "head": {"totals": {"llm_cc": head[0], "token_count": head[1]}}}}
        report.update(changes)
        publication = {"schema_version": 2, "identity": identity,
                       "fingerprint": report["fingerprint"], "status": report["status"],
                       "comment": {"path": "comment.md", "bytes": len(comment),
                                   "sha256": hashlib.sha256(comment).hexdigest()}}
        for name, value in [("report.json", report), ("publication.json", publication)]:
            (directory / name).write_text(json.dumps(value))
        (directory / "comment.md").write_bytes(comment)

    def payload(self, directory):
        comment = (directory / "comment.md").read_bytes()
        publication = json.loads((directory / "publication.json").read_text())
        _check_comment(publication, comment)
        self.assertEqual(comment.count(ratchet.DATA_OPEN.encode()), 1)
        data = comment.decode().split(ratchet.PREFIX, 1)[1].split("`", 1)[0]
        self.assertFalse(any(char in data for char in "@[]<>`"))
        return json.loads(data)

    def test_precise_equal_improved_and_regressed_ratios_keep_summary_first(self):
        for complexity in (10, 5, 10.0000000001):
            with self.subTest(complexity=complexity), tempfile.TemporaryDirectory() as name:
                directory = Path(name)
                self.artifacts(directory, head=(complexity, 100))
                ratchet.annotate(directory)
                payload = self.payload(directory)
                self.assertEqual(payload["base_ratio"], 0.1)
                self.assertEqual(payload["head_ratio"], complexity / 100)
                self.assertEqual(payload["identity"]["target_branch"], "release@`v1")
                comment = (directory / "comment.md").read_bytes()
                self.assertTrue(comment.startswith(b"### llm-cc:"))
                self.assertIn("LM-CC/token 0.1 →".encode(), comment.splitlines()[0])
                self.assertIn(b"runtime +1.0", comment)
                ratchet.annotate(directory)
                self.assertEqual((directory / "comment.md").read_bytes(), comment)

    def test_native_ratio_headline_is_not_duplicated(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            title = "### llm-cc: LM-CC delta · LM-CC/token 0.1 → 0.05 (-50%)\n".encode()
            self.artifacts(directory, head=(5, 100), comment=title)
            ratchet.annotate(directory)
            self.assertEqual((directory / "comment.md").read_bytes().splitlines()[0], title.rstrip())

    def test_missing_zero_incomplete_and_failed_measurements_are_unavailable(self):
        for complexity, tokens, changes in [
            (None, None, {}), (0, 0, {}), (10, 100, {"status": "incomplete"}),
            (10, 100, {"status": "failed"}), (10, 100, {"errors": ["worker failed"]}),
        ]:
            with self.subTest(changes=changes, tokens=tokens), tempfile.TemporaryDirectory() as name:
                directory = Path(name)
                self.artifacts(directory, head=(complexity, tokens), **changes)
                ratchet.annotate(directory)
                self.assertIsNone(self.payload(directory)["head_ratio"])

    def test_invalid_metrics_and_changed_envelopes_fail_before_writes(self):
        for complexity, tokens in [(float("nan"), 100), (float("inf"), 100),
                                   (-1, 100), (1, -1), (1, True), (True, 100)]:
            with self.subTest(complexity=complexity, tokens=tokens), tempfile.TemporaryDirectory() as name:
                directory = Path(name)
                self.artifacts(directory, head=(complexity, tokens))
                before = (directory / "comment.md").read_bytes()
                with self.assertRaises(ValueError):
                    ratchet.annotate(directory)
                self.assertEqual((directory / "comment.md").read_bytes(), before)
        for field, value in [("identity", {}), ("fingerprint", "other"),
                             ("status", "failed"), ("schema_version", 1)]:
            with self.subTest(field=field), tempfile.TemporaryDirectory() as name:
                directory = Path(name)
                self.artifacts(directory)
                path = directory / "publication.json"
                publication = json.loads(path.read_text())
                publication[field] = value
                path.write_text(json.dumps(publication))
                with self.assertRaises(ValueError):
                    ratchet.annotate(directory)
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            self.artifacts(directory)
            (directory / "comment.md").write_bytes(b"tampered")
            with self.assertRaisesRegex(ValueError, "envelope"):
                ratchet.annotate(directory)

    def test_full_comment_keeps_lead_and_payload_within_publication_limit(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            lead = b"### llm-cc: repository LM-CC delta\n\nruntime +1.0"
            start = lead + b"\n\n<details>\n<summary>Full comparison</summary>\n\n"
            end = b"\n</details>\n"
            comment = start + b"x" * (ratchet.MAX_COMMENT_BYTES - len(start) - len(end)) + end
            self.artifacts(directory, comment=comment)
            ratchet.annotate(directory)
            self.payload(directory)
            self.assertIn(b"runtime +1.0", (directory / "comment.md").read_bytes())

    def test_skipped_run_needs_no_report(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            (directory / "skipped.json").write_text("{}")
            ratchet.annotate(directory)

    def test_isolated_entrypoint_ignores_checkout_import_shadowing(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            self.artifacts(directory)
            producer = directory / "ratchet.py"
            producer.write_bytes(Path(ratchet.__file__).read_bytes())
            (directory / "json.py").write_text("raise RuntimeError('untrusted import')\n")
            result = subprocess.run([sys.executable, "-I", str(producer), str(directory)],
                                    cwd=directory, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(self.payload(directory)["head_ratio"], 0.1)
