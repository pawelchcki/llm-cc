import json
import os
import subprocess
import tempfile
import threading
import time
import unittest
from unittest import mock
from pathlib import Path

from tools.comparison.__main__ import parser
from tools.comparison.cache import CacheError, FilesystemStore, ResultCache
from tools.comparison.common import write_json
from tools.comparison.fixtures import git, synthetic_scorer
from tools.comparison.inventory import (
    GitError,
    _classify,
    inventory,
    merge_base,
    resolve_language,
    validate_rules,
)
from tools.comparison.pipeline import (
    REPOSITORY_RULES_PATH,
    _assemble,
    _changed_files,
    _code,
    _delta,
    _rankings,
    _render,
    _safe,
    aggregate,
    compare,
    prepare,
    resolve_rules,
)


class MemoryCache:
    def __init__(self, values=None):
        self.values = values or {}

    def get(self, item, fingerprint):
        return self.values.get(item["key"])


class PipelineTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.repo = Path(self.temp.name) / "repo"
        self.repo.mkdir()
        git(self.repo, "init", "-q")
        (self.repo / "same.cc").write_text("int x;\n")
        (self.repo / "copy.cc").write_text("int x;\n")
        (self.repo / "src_test.cc").write_text("int test;\n")
        (self.repo / "README.md").write_text("docs\n")
        (self.repo / "target.cc").write_text("target.cc")
        (self.repo / "api.hpp").write_text("int api();\n")
        (self.repo / "move.cc").write_text("int move;\n")
        (self.repo / "link.cc").symlink_to("target.cc")
        (self.repo / "odd\nname.py").write_text("x = 1\n")
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-qm", "base")
        self.base = git(self.repo, "rev-parse", "HEAD")
        (self.repo / "same.cc").write_text("int changed;\n")
        (self.repo / "large.py").write_text("x" * 20)
        (self.repo / "tests").mkdir()
        git(self.repo, "mv", "move.cc", "tests/move.cc")
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-qm", "head")
        self.head = git(self.repo, "rev-parse", "HEAD")
        self.profile = {
            "scoring": {"tau": 0.67},
            "build": {"source_commit": "abc"},
            "max_file_bytes": 10,
        }
        self.identity = {
            "repository": "o/r",
            "pipeline_id": "p",
            "target_branch": "main",
            "pr_number": 1,
            "started_at": "2026-01-01T00:00:00Z",
        }

    def tearDown(self):
        self.temp.cleanup()

    def test_report_paths_neutralize_mentions_and_control_characters(self):
        escaped = _safe(
            "src/@team/[file]<tag>\u202e.cc www.example.com https://example.com"
        )
        self.assertIn("&#64;&#8203;team", escaped)
        self.assertIn("&#91;file&#93;&lt;tag&gt;", escaped)
        self.assertNotIn("\u202e", escaped)
        self.assertIn("u202e", escaped)
        self.assertNotIn("www.", escaped)
        self.assertNotIn("https://", escaped)

    def _scored(self, out, tokens=lambda key: 2):
        plan = prepare(
            self.repo,
            self.head,
            self.base,
            self.identity,
            self.profile,
            {},
            MemoryCache(),
            out,
            1,
        )
        worker = {
            "schema_version": 1,
            "identity": plan["identity"],
            "fingerprint": plan["fingerprint"],
            "worker_id": 0,
            "status": "complete",
            "errors": [],
            "elapsed_seconds": 0,
            "results": {},
        }
        for index, key in enumerate(plan["workers"][0]["keys"]):
            item = plan["items"][key]
            worker["results"][key] = {
                "schema_version": 1,
                "key": key,
                "fingerprint": plan["fingerprint"],
                "content_sha256": item["content_sha256"],
                "language": item["language"],
                "llm_cc": 2.0 + index,
                "token_count": tokens(key),
            }
        path = out / "worker-0.json"
        write_json(path, worker)
        return plan, aggregate(out / "plan.json", [path], out / "report")

    def _report(self, **overrides):
        comparison = {
            "score": _delta(1.0, 2.0),
            "raw_llm_cc": _delta(1.0, 2.0),
            "tokens": _delta(1, 1),
            "coverage": {"base": 1.0, "head": 1.0},
        }
        report = {
            "status": "complete",
            "identity": {"target_branch": "main", "repository": "o/r"},
            "comparisons": {
                name: comparison
                for name in ("runtime", "tests", "tooling", "repository")
            },
            "cache_stats": {"hits": 1, "misses": 0},
            "rankings": {"base": [], "head": []},
            "changed_files": [],
            "leading_regressions": [],
            "leading_improvements": [],
            "rules_source": {"source": "host"},
            "presentation": {},
            "errors": [],
        }
        report.update(overrides)
        return report

    def test_raw_lm_cc_is_the_rendered_headline(self):
        markdown = _render(
            self._report(
                leading_regressions=[
                    {"path": "a.cc", "change": 1.0, "raw_change": 2.5},
                    {"path": "legacy.cc", "change": 0.5},
                    {"path": "empty.cc", "change": None, "raw_change": 3.0},
                ]
            )
        )
        self.assertIn("- `empty.cc`: +3.0 LM-CC\n", markdown)
        self.assertIn("- repository 1.0 → 2.0 LM-CC (+100%)", markdown)
        self.assertIn("| repository | 1.0 | 2.0 | +1.0 | +100% |", markdown)
        self.assertIn("- `a.cc`: +2.5 LM-CC (+1 LM-CC/token)", markdown)
        self.assertIn("- `legacy.cc`: +0.5 LM-CC/token", markdown)

    def test_leading_paths_rank_by_raw_lm_cc(self):
        # Admit the modified same.cc on both sides so it carries a delta.
        self.profile["max_file_bytes"] = 100
        plan, report = self._scored(Path(self.temp.name) / "raw")
        leading = report["leading_regressions"] + report["leading_improvements"]
        entry = next(x for x in leading if x["path"] == "same.cc")
        base = next(
            f for f in plan["inventories"]["base"] if f["path"] == "same.cc"
        )
        head = next(
            f for f in plan["inventories"]["head"] if f["path"] == "same.cc"
        )
        keys = plan["workers"][0]["keys"]
        # _scored assigns llm_cc = 2 + worker index over 2 tokens per file.
        raw_base = 2.0 + keys.index(base["key"])
        raw_head = 2.0 + keys.index(head["key"])
        self.assertEqual(entry["raw_change"], raw_head - raw_base)
        self.assertAlmostEqual(entry["change"], (raw_head - raw_base) / 2)

    def test_added_files_carry_their_whole_raw_score(self):
        self.profile["max_file_bytes"] = 100
        plan, report = self._scored(Path(self.temp.name) / "added")
        added = next(f for f in plan["inventories"]["head"] if f["path"] == "large.py")
        self.assertNotIn("large.py", [f["path"] for f in plan["inventories"]["base"]])
        entry = next(
            x
            for x in report["leading_regressions"] + report["leading_improvements"]
            if x["path"] == "large.py"
        )
        self.assertIsNone(entry["change"])
        self.assertEqual(entry["raw_change"], report["results"][added["key"]]["llm_cc"])

    def test_zero_token_files_keep_raw_deltas(self):
        self.profile["max_file_bytes"] = 100
        out = Path(self.temp.name) / "zero"
        plan = prepare(
            self.repo, self.head, self.base, self.identity, self.profile, {},
            MemoryCache(), out / "probe", 1,
        )
        base = next(f for f in plan["inventories"]["base"] if f["path"] == "same.cc")
        head = next(f for f in plan["inventories"]["head"] if f["path"] == "same.cc")
        for label, empty in (("one", {base["key"]}), ("both", {base["key"], head["key"]})):
            with self.subTest(zero_token_sides=label):
                _, report = self._scored(
                    out / label, tokens=lambda key: 0 if key in empty else 2
                )
                leading = report["leading_regressions"] + report["leading_improvements"]
                entry = next(x for x in leading if x["path"] == "same.cc")
                self.assertIsNone(entry["change"])
                self.assertNotEqual(entry["raw_change"], 0)

    def test_untrusted_paths_render_as_balanced_code_spans(self):
        path = "src/@team/[file]`name``x`~~$y$|pipe|https://example.com/a.cc"
        markdown = _render(
            self._report(
                leading_regressions=[{"path": path, "change": 1.0}],
                changed_files=[
                    {
                        "status": "M",
                        "old_path": path,
                        "new_path": path,
                        "path": path,
                        "base": {"category": "runtime", "score": 1.0, "llm_cc": 1.0},
                        "head": {
                            "category": "runtime",
                            "score": 2.0,
                            "llm_cc": 2.0,
                            "rank": 3,
                            "category_rank": 2,
                        },
                        "delta": 1.0,
                        "percent": 100.0,
                        "raw_delta": 1.0,
                        "raw_percent": 100.0,
                    }
                ],
                rankings={
                    "base": [],
                    "head": [{"path": path, "rank": 3, "category_rank": 2}] * 130,
                },
            )
        )
        for line in markdown.splitlines():
            self.assertEqual(
                line.count("`") % 2, 0, "unbalanced code fence in %r" % line
            )
        bullet = next(
            line for line in markdown.splitlines() if "@team" in line and line[0] == "-"
        )
        self.assertTrue(bullet.startswith("- ``"))
        self.assertIn("@team", bullet)
        self.assertIn("[file]", bullet)
        self.assertIn("https://example.com", bullet)
        row = next(line for line in markdown.splitlines() if line.startswith("| ``"))
        # GFM splits table cells on unescaped pipes even inside a code span.
        self.assertIn("\\|pipe\\|", row)
        self.assertEqual(row.count("|") - row.count("\\|"), 8)
        self.assertIn("#3/130", row)

    def test_code_spans_fence_backticks_and_truncate_long_paths(self):
        self.assertEqual(_code("plain"), "`plain`")
        self.assertEqual(_code("a`b"), "``a`b``")
        self.assertEqual(_code("`lead"), "`` `lead ``")
        self.assertEqual(_code("a``b`"), "``` a``b` ```")
        long_path = "d/" + "x" * 300 + ".py"
        rendered = _code(long_path)
        self.assertIn("…", rendered)
        self.assertLessEqual(len(rendered.strip("`")), 121)
        self.assertTrue(rendered.strip("`").startswith("d/xxx"))

    def test_table_cells_escape_pipes_behind_literal_backslashes(self):
        # A backslash run in front of a pipe must be doubled, or GFM consumes
        # the escape and the bare pipe ends the cell mid-path.
        self.assertEqual(_code("a|b", table=True), "`a\\|b`")
        self.assertEqual(_code("a\\|@team.cc", table=True), "`a\\\\\\|@team.cc`")
        self.assertEqual(_code("a\\\\|b", table=True), "`a\\\\\\\\\\|b`")
        # Backslashes not in front of a pipe are literal and stay untouched.
        self.assertEqual(_code("odd\\u000aname.py", table=True), "`odd\\u000aname.py`")
        for path in ("a|b", "a\\|b", "a\\\\|b"):
            with self.subTest(path=path):
                cell = _code(path, table=True)
                # Every pipe carries an odd number of preceding backslashes.
                for index, character in enumerate(cell):
                    if character != "|":
                        continue
                    run = 0
                    while index - run - 1 >= 0 and cell[index - run - 1] == "\\":
                        run += 1
                    self.assertEqual(run % 2, 1, cell)

    def test_report_links_and_repository_rules_are_announced(self):
        markdown = _render(
            self._report(
                presentation={
                    "report_links": {"baseline": "https://ci.example/o/r/baseline.md"}
                },
                rules_source={
                    "source": "repository",
                    "path": ".llm-cc/comparison-rules.json",
                    "commit": "0123456789abcdef",
                },
            )
        )
        self.assertIn("Baseline ranking: <https://ci.example/o/r/baseline.md>", markdown)
        self.assertNotIn("](", markdown)
        self.assertIn("Rules: repository `.llm-cc/comparison-rules.json`@0123456", markdown)
        self.assertIn("Rules: host", _render(self._report()))

    def test_comment_stays_within_the_publication_limit(self):
        paths = ["src/%s/%s.cc" % ("deep" * 20, index) for index in range(3000)]
        rows = [
            {
                "status": "M",
                "old_path": path,
                "new_path": path,
                "path": path,
                "base": {"category": "runtime", "score": 1.0, "llm_cc": 1.0},
                "head": {
                    "category": "runtime",
                    "score": 2.0 + index,
                    "llm_cc": 2.0 + index,
                    "rank": index + 1,
                    "category_rank": index + 1,
                },
                "delta": 1.0 + index,
                "percent": 100.0,
                "raw_delta": 1.0 + index,
                "raw_percent": 100.0,
            }
            for index, path in enumerate(paths)
        ]
        offenders = [
            {
                "path": path,
                "category": "runtime",
                "language": "cpp",
                "size": 10,
                "score": 1.0,
                "llm_cc": 1.0,
                "token_count": 1,
                "rank": index + 1,
                "category_rank": index + 1,
                "changed": True,
            }
            for index, path in enumerate(paths)
        ]
        report = self._report(
            changed_files=rows,
            rankings={"base": offenders, "head": offenders},
            leading_regressions=[{"path": path, "change": 1.0} for path in paths[:10]],
            errors=["e" * 2000] * 20,
        )
        markdown = _render(report)
        self.assertLessEqual(len(markdown.encode("utf-8")), 24576)
        for line in markdown.splitlines():
            self.assertEqual(
                line.count("`") % 2, 0, "unbalanced code fence in %r" % line
            )
        self.assertIn("### Changed files", markdown)
        self.assertIn(
            "_2975 more changed files are listed in the full report._", markdown
        )

    def test_assembly_drops_low_priority_sections_then_whole_lines(self):
        head = [(1, ["## llm-cc comparison", "", "Status: **complete**"])]
        changed = [(4, ["", "### Changed files"] + ["| %s |" % _code("a" * 100)] * 30)]
        offenders = [(6, ["", "### Top offenders on base"] + ["- x"] * 200)]
        payload = _assemble(head + changed + offenders, limit=4096).decode("utf-8")
        self.assertLessEqual(len(payload.encode("utf-8")), 4096)
        self.assertNotIn("Top offenders", payload)
        self.assertIn("### Changed files", payload)
        # A single oversized section is cut on line boundaries, never mid-span.
        crowded = _assemble(
            [(1, ["## llm-cc comparison"] + ["- %s" % _code("b" * 100)] * 200)],
            limit=1024,
        ).decode("utf-8")
        self.assertLessEqual(len(crowded.encode("utf-8")), 1024)
        self.assertTrue(
            crowded.endswith("_Full details are available in artifacts._\n")
        )
        for line in crowded.splitlines():
            self.assertEqual(
                line.count("`") % 2, 0, "unbalanced code fence in %r" % line
            )

    def test_rankings_are_deterministic_and_flag_changed_paths(self):
        out = Path(self.temp.name) / "ranked"
        plan, report = self._scored(out)
        head = report["rankings"]["head"]
        self.assertEqual([entry["rank"] for entry in head], list(range(1, len(head) + 1)))
        self.assertEqual(
            head, sorted(head, key=lambda entry: (-entry["llm_cc"], entry["path"]))
        )
        for category in ("runtime", "tests", "tooling"):
            selected = [x["category_rank"] for x in head if x["category"] == category]
            self.assertEqual(selected, list(range(1, len(selected) + 1)))
        changed = {entry["path"] for entry in head if entry["changed"]}
        self.assertIn("tests/move.cc", changed)
        self.assertNotIn("copy.cc", changed)
        moved = next(
            row for row in report["changed_files"] if row["path"] == "tests/move.cc"
        )
        self.assertEqual(moved["head"]["rank"], moved["head"]["rank"])
        self.assertIsNotNone(moved["head"]["category_rank"])
        self.assertEqual(moved["base"]["category"], "runtime")
        self.assertEqual(moved["head"]["category"], "tests")

    def test_zero_base_score_reports_no_percentage(self):
        row = _delta(0.0, 1.0)
        self.assertIsNone(row["percent"])
        self.assertEqual(row["absolute"], 1.0)

    def test_file_tables_and_rankings_follow_total_lm_cc_when_density_rises(self):
        def file(path, key):
            return dict(path=path, key=key, category="tests", language="c",
                        size=100, scorable=True)

        base = file("test_url_parse.c", "base")
        head = file("test_url_parse.c", "head")
        small = file("small.c", "small")
        large = file("large.c", "large")
        empty = file("empty.c", "empty")
        results = {
            "base": {"llm_cc": 48.2, "token_count": 1431},
            "head": {"llm_cc": 45.4, "token_count": 663},
            "small": {"llm_cc": 2.0, "token_count": 2},
            "large": {"llm_cc": 100.0, "token_count": 10000},
            "empty": {"llm_cc": 0.0, "token_count": 0},
        }
        paths = {"test_url_parse.c"}
        rankings = {
            "base": _rankings([base, small, large, empty], results, paths),
            "head": _rankings([head, small, large, empty], results, paths),
        }
        self.assertEqual([r["path"] for r in rankings["head"]],
                         ["large.c", "test_url_parse.c", "small.c", "empty.c"])
        self.assertIsNone(rankings["head"][-1]["score"])
        rows = _changed_files(
            [{"status": "M", "old_path": base["path"], "new_path": head["path"]}],
            {base["path"]: base}, {head["path"]: head}, results, rankings["head"],
        )
        self.assertAlmostEqual(rows[0]["raw_delta"], -2.8)
        self.assertGreater(rows[0]["delta"], 0)
        markdown = _render(self._report(changed_files=rows, rankings=rankings))
        self.assertIn("| LM-CC base | LM-CC head | LM-CC Δ | LM-CC Δ% |", markdown)
        self.assertIn("| `test_url_parse.c` | tests | 48.2 | 45.4 | -2.8 | -5.81% | #2/4 |",
                      markdown)
        self.assertIn("| 1 | `large.c` | tests | 100.0 |", markdown)
        self.assertIn("| # | Path | Category | LM-CC | Touched |", markdown)

    def test_changed_file_raw_metrics_preserve_missing_and_zero_token_states(self):
        def side(key):
            return {"key": key, "path": key, "category": "tests"}

        old, new = side("base"), side("head")
        change = {"status": "M", "old_path": "base", "new_path": "head"}
        rows = _changed_files(
            [change], {"base": old}, {"head": new},
            {"head": {"llm_cc": 0.0, "token_count": 0}}, [],
        )
        self.assertIsNone(rows[0]["base"]["llm_cc"])
        self.assertEqual(rows[0]["head"]["llm_cc"], 0.0)
        self.assertIsNone(rows[0]["raw_delta"])
        self.assertIsNone(rows[0]["head"]["score"])

    def test_repository_rules_come_from_the_target_commit(self):
        (self.repo / ".llm-cc").mkdir()
        (self.repo / ".llm-cc/comparison-rules.json").write_text(
            json.dumps({"tooling": ["copy.cc"]})
        )
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-qm", "target rules")
        target = git(self.repo, "rev-parse", "HEAD")
        (self.repo / ".llm-cc/comparison-rules.json").write_text(
            json.dumps({"exclude": ["**"]})
        )
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-qm", "head rules")
        head = git(self.repo, "rev-parse", "HEAD")
        rules, source = resolve_rules(
            self.repo, target, {"tests": ["nothing"]}, ".llm-cc/comparison-rules.json"
        )
        self.assertEqual(rules, {"tooling": ["copy.cc"]})
        self.assertEqual(source["source"], "repository")
        self.assertEqual(source["commit"], target)
        plan = prepare(
            self.repo,
            head,
            target,
            self.identity,
            self.profile,
            {},
            MemoryCache(),
            Path(self.temp.name) / "repo-rules",
            1,
        )
        self.assertEqual(plan["rules_source"]["source"], "repository")
        by_path = {x["path"]: x for x in plan["inventories"]["head"]}
        self.assertEqual(by_path["copy.cc"]["category"], "tooling")
        missing, host = resolve_rules(self.repo, target, {"tests": ["x"]}, "absent.json")
        self.assertEqual(host, {"source": "host"})
        self.assertEqual(missing, {"tests": ["x"]})

    def test_rules_match_like_llm_cc(self):
        # Globs are segment-aware, and omitted lists keep llm-cc's defaults.
        partial = {"tooling": ["src/*.cc"]}
        self.assertEqual(_classify("src/a.cc", partial), "tooling")
        self.assertEqual(_classify("src/nested/a.cc", partial), "runtime")
        self.assertEqual(_classify("src/a_test.cc", partial), "tests")
        self.assertEqual(_classify("tools/gen.py", {"tests": []}), "tooling")
        (self.repo / "node_modules").mkdir()
        (self.repo / "node_modules/dep.js").write_text("x;\n")
        (self.repo / "keep.js").write_text("x;\n")
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-qm", "dependencies")
        head = git(self.repo, "rev-parse", "HEAD")
        by_path = {
            x["path"]: x for x in inventory(self.repo, head, "f" * 64, partial)[0]
        }
        self.assertEqual(by_path["node_modules/dep.js"]["reason"], "excluded")
        self.assertIsNone(by_path["keep.js"]["reason"])
        overridden, _ = inventory(self.repo, head, "f" * 64, {"exclude": []})
        self.assertIsNone(
            {x["path"]: x for x in overridden}["node_modules/dep.js"]["reason"]
        )

    def test_rules_reject_what_llm_cc_rejects(self):
        for pattern in ("foo/", "/foo", "foo//bar", "a/./b", "x\\", "[ab"):
            with self.subTest(pattern=pattern), self.assertRaises(ValueError):
                validate_rules({"exclude": [pattern]})
        (self.repo / ".llm-cc").mkdir()
        (self.repo / ".llm-cc/rules.json").write_text(
            "{" + " " * (64 * 1024) + '"tests": []}'
        )
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-qm", "padded rules")
        target = git(self.repo, "rev-parse", "HEAD")
        with self.assertRaisesRegex(ValueError, "64 KiB"):
            resolve_rules(self.repo, target, {}, REPOSITORY_RULES_PATH)

    def test_selection_matches_llm_cc_edge_cases(self):
        (self.repo / ".llm-cc-cache").mkdir()
        (self.repo / ".llm-cc-cache/cached.py").write_text("x = 1\n")
        (self.repo / "project-env/lib").mkdir(parents=True)
        (self.repo / "project-env/pyvenv.cfg").write_text("home = /usr\n")
        (self.repo / "project-env/lib/site.py").write_text("x = 1\n")
        kelvin = "foo.\u212aS"  # a Kelvin sign lowercases to "k" in Unicode
        (self.repo / kelvin).write_text("x\n")
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-qm", "edge cases")
        head = git(self.repo, "rev-parse", "HEAD")
        rules = {"extensions": {".ks": "python"}}
        rows = {x["path"]: x for x in inventory(self.repo, head, "f" * 64, rules)[0]}
        self.assertEqual(rows[".llm-cc-cache/cached.py"]["reason"], "excluded")
        self.assertEqual(rows["project-env/lib/site.py"]["reason"], "excluded")
        self.assertIsNone(rows[kelvin]["language"])
        # A reversed range matches nothing, and its negation any character.
        self.assertEqual(_classify("q.cc", {"tests": ["[z-a].cc"]}), "runtime")
        self.assertEqual(_classify("q.cc", {"tests": ["[!z-a].cc"]}), "tests")
        # A set never spans segments, and matching never backtracks forever.
        self.assertEqual(
            _classify("foo/bar.py", {"tests": ["foo[.-0]bar.py"]}), "runtime"
        )
        started = time.monotonic()
        self.assertEqual(_classify("a" * 40, {"tests": ["*a" * 15 + "b"]}), "runtime")
        self.assertLess(time.monotonic() - started, 1)

    def test_committed_rules_are_read_like_llm_cc(self):
        (self.repo / ".llm-cc").mkdir()
        (self.repo / ".llm-cc/rules.json").write_bytes(
            b'\xef\xbb\xbf{"tests": ["copy.cc"]}'
        )
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-qm", "rules with a byte order mark")
        target = git(self.repo, "rev-parse", "HEAD")
        rules, _ = resolve_rules(self.repo, target, {}, REPOSITORY_RULES_PATH)
        self.assertEqual(rules, {"tests": ["copy.cc"]})
        (self.repo / ".llm-cc/rules.json").unlink()
        (self.repo / ".llm-cc/real.json").write_text("{}")
        (self.repo / ".llm-cc/rules.json").symlink_to("real.json")
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-qm", "symlinked rules")
        linked = git(self.repo, "rev-parse", "HEAD")
        with self.assertRaises(GitError):
            resolve_rules(self.repo, linked, {}, REPOSITORY_RULES_PATH)
        # A lone surrogate escape is invalid JSON for llm-cc.
        (self.repo / ".llm-cc/rules.json").unlink()
        (self.repo / ".llm-cc/rules.json").write_text('{"exclude": ["\\ud800"]}')
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-qm", "surrogate rules")
        surrogate = git(self.repo, "rev-parse", "HEAD")
        with self.assertRaises(ValueError):
            resolve_rules(self.repo, surrogate, {}, REPOSITORY_RULES_PATH)
        # A .llm-cc that is not a directory obstructs the rules, never hides them.
        git(self.repo, "rm", "-rq", ".llm-cc")
        (self.repo / ".llm-cc").write_text("not a directory\n")
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-qm", "obstructed rules")
        obstructed = git(self.repo, "rev-parse", "HEAD")
        with self.assertRaises(GitError):
            resolve_rules(self.repo, obstructed, {}, REPOSITORY_RULES_PATH)

    def test_repository_rules_prefer_the_shared_file_name(self):
        (self.repo / ".llm-cc").mkdir()
        (self.repo / ".llm-cc/comparison-rules.json").write_text(
            json.dumps({"tooling": ["copy.cc"]})
        )
        (self.repo / ".llm-cc/rules.json").write_text(
            json.dumps({"tests": ["copy.cc"]})
        )
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-qm", "both rules files")
        target = git(self.repo, "rev-parse", "HEAD")
        rules, source = resolve_rules(self.repo, target, {}, REPOSITORY_RULES_PATH)
        self.assertEqual(rules, {"tests": ["copy.cc"]})
        self.assertEqual(source["path"], ".llm-cc/rules.json")
        git(self.repo, "rm", "-q", ".llm-cc/rules.json")
        git(self.repo, "commit", "-qm", "legacy rules only")
        legacy = git(self.repo, "rev-parse", "HEAD")
        rules, source = resolve_rules(self.repo, legacy, {}, REPOSITORY_RULES_PATH)
        self.assertEqual(rules, {"tooling": ["copy.cc"]})
        self.assertEqual(source["path"], ".llm-cc/comparison-rules.json")

    def test_invalid_repository_rules_fail_the_run(self):
        (self.repo / ".llm-cc").mkdir()
        (self.repo / ".llm-cc/comparison-rules.json").write_text(
            json.dumps({"tests": ["ok"], "unexpected": True})
        )
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-qm", "bad rules")
        target = git(self.repo, "rev-parse", "HEAD")
        with self.assertRaisesRegex(ValueError, "unsupported classification rule keys"):
            resolve_rules(self.repo, target, {}, ".llm-cc/comparison-rules.json")
        (self.repo / ".llm-cc/comparison-rules.json").write_text("{not json")
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-qm", "unparsable rules")
        broken = git(self.repo, "rev-parse", "HEAD")
        with self.assertRaisesRegex(ValueError, "not valid JSON"):
            resolve_rules(self.repo, broken, {}, ".llm-cc/comparison-rules.json")
        with self.assertRaisesRegex(ValueError, "unsupported language"):
            validate_rules({"extensions": {".zig": "zig"}})
        with self.assertRaisesRegex(ValueError, "512 glob"):
            validate_rules({"tests": ["a"] * 513})

    def test_inventory_dedup_categories_and_oversize(self):
        rows, _ = inventory(self.repo, self.head, "f", {"exclude": ["copy.cc"]}, 10)
        by_path = {x["path"]: x for x in rows}
        self.assertEqual(by_path["src_test.cc"]["category"], "tests")
        self.assertEqual(by_path["large.py"]["reason"], "oversized")
        self.assertFalse(by_path["README.md"]["scorable"])
        self.assertFalse(by_path["copy.cc"]["scorable"])
        self.assertEqual(by_path["link.cc"]["reason"], "symlink")
        self.assertFalse(by_path["link.cc"]["scorable"])
        self.assertTrue(by_path["odd\nname.py"]["scorable"])
        self.assertEqual(by_path["api.hpp"]["language"], "cpp")

    def test_inventory_preserves_submodules_as_unmeasured_non_source_entries(self):
        git(
            self.repo,
            "update-index",
            "--add",
            "--cacheinfo",
            "160000",
            self.base,
            "nested-module",
        )
        git(self.repo, "commit", "-qm", "submodule")
        rows, _ = inventory(self.repo, "HEAD", "f")
        module = next(row for row in rows if row["path"] == "nested-module")
        self.assertEqual(module["reason"], "submodule")
        self.assertIsNone(module["size"])
        self.assertFalse(module["scorable"])

    def test_git_path_rules_do_not_inherit_host_case_folding(self):
        # Windows fnmatch normally lowercases both patterns and filenames. Git
        # object paths must retain the same category/exclusion rules on every OS.
        with mock.patch("os.path.normcase", side_effect=lambda value: value.lower()):
            self.assertEqual(
                _classify("Tests/main.py", {"tests": ["tests/**"]}), "runtime"
            )
            rows, _ = inventory(self.repo, self.head, "f", {"exclude": ["COPY.cc"]})
        self.assertTrue(
            next(row for row in rows if row["path"] == "copy.cc")["scorable"]
        )

    def test_prepare_deduplicates_and_zero_hit_workers(self):
        out = Path(self.temp.name) / "out"
        plan = prepare(
            self.repo,
            self.head,
            self.base,
            self.identity,
            self.profile,
            {},
            MemoryCache(),
            out,
            4,
        )
        self.assertEqual(plan["identity"]["base_sha"], self.base)
        self.assertLess(
            len(plan["items"]),
            sum(x["scorable"] for side in plan["inventories"].values() for x in side),
        )
        values = {
            key: {
                "schema_version": 1,
                "key": key,
                "fingerprint": plan["fingerprint"],
                "content_sha256": item["content_sha256"],
                "language": item["language"],
                "llm_cc": 1.0,
                "token_count": 1,
            }
            for key, item in plan["items"].items()
        }
        warm = prepare(
            self.repo,
            self.head,
            self.base,
            self.identity,
            self.profile,
            {},
            MemoryCache(values),
            Path(self.temp.name) / "warm",
            4,
        )
        self.assertEqual(warm["workers"], [])
        self.assertEqual(warm["cache_stats"]["misses"], 0)

    def test_repeated_preparation_repairs_truncated_input_blobs(self):
        output = Path(self.temp.name) / "reused-preparation"
        arguments = (
            self.repo,
            self.head,
            self.base,
            self.identity,
            self.profile,
            {},
            MemoryCache(),
            output,
            1,
        )
        first = prepare(*arguments)
        expected = {
            item["blob"]: (output / item["blob"]).read_bytes()
            for item in first["items"].values()
        }
        corrupted = next(iter(expected))
        (output / corrupted).write_bytes(expected[corrupted][:-1])
        repeated = prepare(*arguments)
        self.assertEqual(repeated, first)
        for relative, content in expected.items():
            self.assertEqual((output / relative).read_bytes(), content)

    def test_legacy_container_profile_cannot_reuse_cached_results(self):
        profile = dict(
            self.profile,
            build=dict(
                self.profile["build"],
                execution_image="registry.example/scorer@sha256:" + "a" * 64,
            ),
        )
        cache = mock.Mock()
        cache.get.side_effect = lambda item, fingerprint: dict(
            item,
            schema_version=1,
            fingerprint=fingerprint,
            llm_cc=1.0,
            token_count=1,
        )
        with self.assertRaises(ValueError):
            prepare(
                self.repo,
                self.head,
                self.base,
                self.identity,
                profile,
                {},
                cache,
                Path(self.temp.name) / "legacy",
                1,
            )
        cache.get.assert_not_called()

    def test_worker_partition_is_stable_across_counts_and_reversed_sides(self):
        one = prepare(
            self.repo,
            self.head,
            self.base,
            self.identity,
            self.profile,
            {},
            MemoryCache(),
            Path(self.temp.name) / "one",
            1,
        )
        four = prepare(
            self.repo,
            self.head,
            self.base,
            self.identity,
            self.profile,
            {},
            MemoryCache(),
            Path(self.temp.name) / "four",
            4,
        )
        reversed_plan = prepare(
            self.repo,
            self.base,
            self.head,
            self.identity,
            self.profile,
            {},
            MemoryCache(),
            Path(self.temp.name) / "reverse",
            4,
        )

        def keys(plan):
            return {key for worker in plan["workers"] for key in worker["keys"]}

        self.assertEqual(keys(one), keys(four))
        self.assertEqual(keys(four), keys(reversed_plan))
        again = prepare(
            self.repo,
            self.head,
            self.base,
            self.identity,
            self.profile,
            {},
            MemoryCache(),
            Path(self.temp.name) / "again",
            4,
        )
        self.assertEqual(four["workers"], again["workers"])

    def test_aggregate_success_and_failure_artifacts(self):
        out = Path(self.temp.name) / "prep"
        plan = prepare(
            self.repo,
            self.head,
            self.base,
            self.identity,
            self.profile,
            {},
            MemoryCache(),
            out,
            1,
        )
        worker = {
            "schema_version": 1,
            "identity": plan["identity"],
            "fingerprint": plan["fingerprint"],
            "worker_id": 0,
            "status": "complete",
            "errors": [],
            "elapsed_seconds": 0,
            "results": {},
        }
        for key in plan["workers"][0]["keys"]:
            item = plan["items"][key]
            worker["results"][key] = {
                "schema_version": 1,
                "key": key,
                "fingerprint": plan["fingerprint"],
                "content_sha256": item["content_sha256"],
                "language": item["language"],
                "llm_cc": 2.0,
                "token_count": 2,
            }
        worker_path = out / "worker-0.json"
        write_json(worker_path, worker)
        report = aggregate(out / "plan.json", [worker_path], out / "report")
        self.assertEqual(report["status"], "incomplete")
        self.assertIsNone(report["comparisons"]["repository"]["score"]["head"])
        self.assertIsNone(report["comparisons"]["repository"]["raw_llm_cc"]["head"])
        self.assertIsNone(report["comparisons"]["repository"]["tokens"]["head"])
        self.assertLess(report["sides"]["head"]["totals"]["coverage"], 1.0)
        self.assertLess(report["sides"]["head"]["totals"]["byte_coverage"], 1.0)
        self.assertEqual(report["change_counts"]["renames"], 1)
        self.assertEqual(report["change_counts"]["category_moves"], 1)
        self.assertLessEqual((out / "report/comment.md").stat().st_size, 24 * 1024)
        self.assertIn(
            "`odd\\u000aname.py`", (out / "report/report.md").read_text()
        )
        self.assertIn(
            "Baseline ranking for o/r@", (out / "report/baseline.md").read_text()
        )
        baseline = json.loads((out / "report/baseline.json").read_text())
        self.assertEqual(baseline["schema_version"], 1)
        self.assertEqual(baseline["rankings"], report["rankings"]["head"])
        bad = aggregate(out / "plan.json", [], out / "failed")
        self.assertEqual(bad["status"], "failed")
        self.assertTrue((out / "failed/publication.json").exists())

    def test_invalid_plan_and_failed_worker_produce_failure_reports(self):
        out = Path(self.temp.name) / "invalid"
        out.mkdir()
        (out / "plan.json").write_text("{bad")
        malformed = aggregate(out / "plan.json", [], out / "malformed")
        self.assertEqual(malformed["status"], "failed")
        self.assertTrue((out / "malformed/comment.md").exists())

        prepared = Path(self.temp.name) / "prepared"
        plan = prepare(
            self.repo,
            self.head,
            self.base,
            self.identity,
            self.profile,
            {},
            MemoryCache(),
            prepared,
            1,
        )
        failed = {
            "schema_version": 1,
            "identity": plan["identity"],
            "fingerprint": plan["fingerprint"],
            "worker_id": 0,
            "status": "failed",
            "errors": ["boom"],
            "results": {},
            "elapsed_seconds": 0,
        }
        path = prepared / "worker-0.json"
        write_json(path, failed)
        report = aggregate(prepared / "plan.json", [path], prepared / "failed-report")
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["results"], {})

        # Structurally corrupt worker JSON must not discard the valid plan's
        # inventory/coverage, or any independently validated worker results.
        for malformed_artifact in (
            [],
            dict(failed, results=None),
            dict(failed, worker_id=False),
        ):
            write_json(path, malformed_artifact)
            report = aggregate(
                prepared / "plan.json", [path], prepared / "malformed-worker-report"
            )
            self.assertEqual(report["status"], "failed")
            self.assertEqual(report["inventories"], plan["inventories"])
            self.assertIn("malformed worker artifact", report["errors"][0])

        tampered = dict(plan)
        tampered["fingerprint"] = "0" * 64
        write_json(prepared / "tampered.json", tampered)
        rejected = aggregate(
            prepared / "tampered.json", [], prepared / "tampered-report"
        )
        self.assertEqual(rejected["status"], "failed")
        self.assertIn("fingerprint", rejected["errors"][0])

        wrong = json.loads(json.dumps(plan))
        wrong_key = wrong["workers"][0]["keys"].pop()
        item = wrong["items"][wrong_key]
        wrong["hits"][wrong_key] = {
            "schema_version": 1,
            "key": wrong_key,
            "fingerprint": "bad",
            "content_sha256": item["content_sha256"],
            "language": item["language"],
            "llm_cc": 1.0,
            "token_count": 1,
        }
        write_json(prepared / "invalid-hit.json", wrong)
        invalid_hit = aggregate(
            prepared / "invalid-hit.json", [], prepared / "invalid-hit-report"
        )
        self.assertEqual(invalid_hit["status"], "failed")
        self.assertIn("cached result", invalid_hit["errors"][0])

        wrong_identity = dict(failed)
        wrong_identity["status"] = "complete"
        wrong_identity["errors"] = []
        wrong_identity["identity"] = dict(plan["identity"], pipeline_id="another")
        write_json(prepared / "wrong-identity.json", wrong_identity)
        rejected_worker = aggregate(
            prepared / "plan.json",
            [prepared / "wrong-identity.json"],
            prepared / "wrong-worker-report",
        )
        self.assertEqual(rejected_worker["status"], "failed")
        self.assertEqual(rejected_worker["results"], {})

    def test_missing_history_is_actionable(self):
        with self.assertRaisesRegex(GitError, "fetch.*deepen"):
            merge_base(self.repo, "missing", self.head)

    def test_empty_revisions_and_zero_baselines_are_unavailable(self):
        empty = Path(self.temp.name) / "empty"
        empty.mkdir()
        git(empty, "init", "-q")
        git(empty, "commit", "--allow-empty", "-qm", "empty-base")
        base = git(empty, "rev-parse", "HEAD")
        git(empty, "commit", "--allow-empty", "-qm", "empty-head")
        head = git(empty, "rev-parse", "HEAD")
        plan = prepare(
            empty,
            head,
            base,
            self.identity,
            self.profile,
            {},
            MemoryCache(),
            Path(self.temp.name) / "empty-out",
            4,
        )
        report = aggregate(
            Path(self.temp.name) / "empty-out/plan.json",
            [],
            Path(self.temp.name) / "empty-report",
        )
        self.assertEqual(plan["workers"], [])
        self.assertEqual(report["status"], "complete")
        self.assertIsNone(report["comparisons"]["repository"]["score"]["head"])
        self.assertIsNone(_delta(0.0, 1.0)["percent"])

    def test_compare_cold_warm_incremental_and_worker_count_equality(self):
        root = Path(self.temp.name)
        scorer, model, profile = synthetic_scorer(root)
        cache = ResultCache(FilesystemStore(root / "cache"))
        cold = compare(
            self.repo,
            self.head,
            self.base,
            self.identity,
            profile,
            {},
            cache,
            root / "cold",
            scorer,
            model,
            root,
            1,
        )
        self.assertEqual(cold["status"], "complete", cold["errors"])
        self.assertGreater(cold["cache_stats"]["misses"], 0)
        warm = compare(
            self.repo,
            self.head,
            self.base,
            self.identity,
            profile,
            {},
            cache,
            root / "warm",
            scorer,
            model,
            root,
            4,
        )
        self.assertEqual(warm["cache_stats"]["misses"], 0)
        self.assertEqual(cold["comparisons"], warm["comparisons"])

        (self.repo / "incremental.go").write_text("package p\n")
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-qm", "incremental")
        newer = git(self.repo, "rev-parse", "HEAD")
        incremental = compare(
            self.repo,
            newer,
            self.base,
            self.identity,
            profile,
            {},
            cache,
            root / "incremental",
            scorer,
            model,
            root,
            4,
        )
        self.assertEqual(incremental["cache_stats"]["misses"], 1)
        fresh = ResultCache(FilesystemStore(root / "fresh-cache"))
        four = compare(
            self.repo,
            newer,
            self.base,
            self.identity,
            profile,
            {},
            fresh,
            root / "four-workers",
            scorer,
            model,
            root,
            4,
        )
        self.assertEqual(incremental["comparisons"], four["comparisons"])


if __name__ == "__main__":
    unittest.main()


class PathLanguageRulesTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name) / "repo"
        (self.repo / "c").mkdir(parents=True)
        (self.repo / "cpp").mkdir(parents=True)
        git(self.repo, "init", "-q")
        (self.repo / "c/x.h").write_text("int shared(void);\n")
        (self.repo / "cpp/x.h").write_text("int shared(void);\n")
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-qm", "base")
        self.base = git(self.repo, "rev-parse", "HEAD")
        self.head = self.base
        self.rules = {
            "paths": [
                {"pattern": "cpp/**", "language": "cpp"},
                {"pattern": "c/**", "language": "c"},
            ]
        }
        self.profile = {
            "scoring": {"tau": 0.67},
            "build": {"source_commit": "abc"},
            "max_file_bytes": 1024,
        }
        self.identity = {
            "repository": "o/r",
            "pipeline_id": "p",
            "target_branch": "main",
            "pr_number": 1,
            "started_at": "2026-01-01T00:00:00Z",
        }

    def _prepare(self, name, rules=None, head=None, target=None):
        return prepare(
            self.repo,
            head or self.head,
            target or self.base,
            self.identity,
            self.profile,
            self.rules if rules is None else rules,
            MemoryCache(),
            Path(self.temp.name) / name,
            1,
        )

    def test_path_rules_select_language_before_keys(self):
        plan = self._prepare("paths")
        by_path = {record["path"]: record for record in plan["inventories"]["head"]}
        self.assertEqual(by_path["c/x.h"]["language"], "c")
        self.assertEqual(by_path["cpp/x.h"]["language"], "cpp")
        self.assertEqual(
            by_path["c/x.h"]["content_sha256"], by_path["cpp/x.h"]["content_sha256"]
        )
        self.assertNotEqual(by_path["c/x.h"]["key"], by_path["cpp/x.h"]["key"])
        self.assertEqual(len(plan["items"]), 2)
        default = self._prepare("default", rules={})
        defaults = {record["path"]: record for record in default["inventories"]["head"]}
        self.assertEqual(defaults["cpp/x.h"]["language"], "c")
        self.assertEqual(len(default["items"]), 1)

    def test_overlapping_path_rules_first_match_wins_over_extensions(self):
        rules = {
            "paths": [
                {"pattern": "cpp/x.h", "language": "cpp"},
                {"pattern": "cpp/**", "language": "rust"},
            ],
            "extensions": {".h": "python"},
        }
        plan = self._prepare("precedence", rules=rules)
        by_path = {record["path"]: record for record in plan["inventories"]["head"]}
        self.assertEqual(by_path["cpp/x.h"]["language"], "cpp")
        self.assertEqual(by_path["c/x.h"]["language"], "python")
        self.assertEqual(
            resolve_language("other/x.h", {"extensions": {".h": "python"}}), "python"
        )
        self.assertEqual(resolve_language("other/x.h", {}), "c")

    def test_rename_between_language_overridden_directories(self):
        (self.repo / "c/moved.h").write_text("int moved(void);\n")
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-qm", "add movable header")
        base = git(self.repo, "rev-parse", "HEAD")
        git(self.repo, "mv", "c/moved.h", "cpp/moved.h")
        git(self.repo, "commit", "-qm", "move header")
        head = git(self.repo, "rev-parse", "HEAD")
        plan = self._prepare("rename", head=head, target=base)
        statuses = {
            (change["old_path"], change["new_path"]): change["status"]
            for change in plan["changes"]
        }
        self.assertIn(("c/moved.h", "cpp/moved.h"), statuses)
        self.assertTrue(statuses[("c/moved.h", "cpp/moved.h")].startswith("R"))
        base_records = {r["path"]: r for r in plan["inventories"]["base"]}
        head_records = {r["path"]: r for r in plan["inventories"]["head"]}
        self.assertEqual(base_records["c/moved.h"]["language"], "c")
        self.assertEqual(head_records["cpp/moved.h"]["language"], "cpp")
        base_key = base_records["c/moved.h"]["key"]
        head_key = head_records["cpp/moved.h"]["key"]
        self.assertNotEqual(base_key, head_key)
        results = {
            key: {
                "schema_version": 1,
                "key": key,
                "fingerprint": plan["fingerprint"],
                "content_sha256": plan["items"][key]["content_sha256"],
                "language": plan["items"][key]["language"],
                "llm_cc": 4.0 if key == head_key else 2.0,
                "token_count": 2,
            }
            for key in plan["items"]
        }
        rows = _changed_files(
            plan["changes"],
            {record["path"]: record for record in plan["inventories"]["base"]},
            {record["path"]: record for record in plan["inventories"]["head"]},
            results,
            [],
        )
        moved = [row for row in rows if row["path"] == "cpp/moved.h"]
        self.assertEqual(len(moved), 1)
        self.assertEqual(moved[0]["base"]["score"], 1.0)
        self.assertEqual(moved[0]["head"]["score"], 2.0)

    def test_path_rules_apply_to_both_revisions_from_target_rules(self):
        (self.repo / ".llm-cc").mkdir()
        (self.repo / ".llm-cc/comparison-rules.json").write_text(json.dumps(self.rules))
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-qm", "target rules")
        target = git(self.repo, "rev-parse", "HEAD")
        (self.repo / ".llm-cc/comparison-rules.json").write_text(
            json.dumps({"paths": [{"pattern": "**", "language": "rust"}]})
        )
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-qm", "head rules")
        head = git(self.repo, "rev-parse", "HEAD")
        plan = prepare(
            self.repo,
            head,
            target,
            self.identity,
            self.profile,
            {},
            MemoryCache(),
            Path(self.temp.name) / "target-rules",
            1,
        )
        self.assertEqual(plan["rules_source"]["source"], "repository")
        for side in ("base", "head"):
            by_path = {record["path"]: record for record in plan["inventories"][side]}
            self.assertEqual(by_path["c/x.h"]["language"], "c")
            self.assertEqual(by_path["cpp/x.h"]["language"], "cpp")

    @unittest.skipUnless(os.name == "posix", "symlink creation requires POSIX")
    def test_path_rules_do_not_follow_symlinks_or_submodules(self):
        (self.repo / "cpp/link.h").symlink_to("x.h")
        subprocess.run(
            ["git", "-C", str(self.repo), "update-index", "--add", "--cacheinfo",
             "160000,%s,cpp/module" % self.base],
            check=True,
        )
        git(self.repo, "add", "cpp/link.h")
        git(self.repo, "commit", "-qm", "link and submodule")
        head = git(self.repo, "rev-parse", "HEAD")
        plan = self._prepare("symlinks", head=head)
        by_path = {record["path"]: record for record in plan["inventories"]["head"]}
        self.assertEqual(by_path["cpp/link.h"]["reason"], "symlink")
        self.assertFalse(by_path["cpp/link.h"]["scorable"])
        self.assertEqual(by_path["cpp/module"]["reason"], "submodule")
        self.assertFalse(by_path["cpp/module"]["scorable"])

    def test_invalid_path_rules_rejected(self):
        for paths in (
            {},
            "cpp/**",
            [["cpp/**", "cpp"]],
            [{"pattern": "cpp/**"}],
            [{"language": "cpp"}],
            [{"pattern": "cpp/**", "language": "cpp", "extra": 1}],
            [{"pattern": "", "language": "cpp"}],
            [{"pattern": "c" * 257, "language": "cpp"}],
            [{"pattern": "cpp/**", "language": "brainfuck"}],
            [{"pattern": "cpp/**", "language": None}],
            [{"pattern": 7, "language": "cpp"}],
        ):
            with self.subTest(paths=paths), self.assertRaises(ValueError):
                validate_rules({"paths": paths})
        many = [{"pattern": "p%d" % index, "language": "c"} for index in range(500)]
        validate_rules({"paths": many})
        with self.assertRaisesRegex(ValueError, "512 glob patterns"):
            validate_rules({"paths": many, "exclude": ["x"] * 13})


class DelayedFailingCache(MemoryCache):
    """Counts concurrent lookups and can fail or corrupt individual keys."""

    def __init__(self, values=None, delay=0.0, failing=(), invalid=()):
        super().__init__(values)
        self.delay = delay
        self.failing = set(failing)
        self.invalid = set(invalid)
        self.lock = threading.Lock()
        self.started = 0
        self.completed = 0
        self.in_flight = 0
        self.max_in_flight = 0

    def get(self, item, fingerprint):
        key = item["key"]
        with self.lock:
            self.started += 1
            self.in_flight += 1
            self.max_in_flight = max(self.max_in_flight, self.in_flight)
        try:
            time.sleep(self.delay)
            if key in self.failing:
                raise CacheError("store unavailable")
            if key in self.invalid:
                return {"schema_version": 1, "key": "wrong"}
            with self.lock:
                self.completed += 1
            return self.values.get(key)
        finally:
            with self.lock:
                self.in_flight -= 1


class PrepareConcurrencyTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name) / "repo"
        self.repo.mkdir()
        git(self.repo, "init", "-q")
        for index in range(6):
            (self.repo / ("file%d.cc" % index)).write_text("int v%d;\n" % index)
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-qm", "base")
        self.base = git(self.repo, "rev-parse", "HEAD")
        (self.repo / "file0.cc").write_text("int changed;\n")
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-qm", "head")
        self.head = git(self.repo, "rev-parse", "HEAD")
        self.profile = {
            "scoring": {"tau": 0.67},
            "build": {"source_commit": "abc"},
            "max_file_bytes": 1024,
        }
        self.identity = {
            "repository": "o/r",
            "pipeline_id": "p",
            "target_branch": "main",
            "pr_number": 1,
            "started_at": "2026-01-01T00:00:00Z",
        }

    def _results(self, keys, plan):
        return {
            key: {
                "schema_version": 1,
                "key": key,
                "fingerprint": plan["fingerprint"],
                "content_sha256": plan["items"][key]["content_sha256"],
                "language": plan["items"][key]["language"],
                "llm_cc": 1.0,
                "token_count": 1,
            }
            for key in keys
        }

    def _prepare(self, cache, name, concurrency):
        return prepare(
            self.repo,
            self.head,
            self.base,
            self.identity,
            self.profile,
            {},
            cache,
            Path(self.temp.name) / name,
            1,
            cache_concurrency=concurrency,
        )

    def test_concurrent_plan_equals_sequential_plan(self):
        cold = self._prepare(MemoryCache(), "cold", 1)
        values = self._results(cold["items"], cold)
        sequential = self._prepare(DelayedFailingCache(values), "sequential", 1)
        concurrent_plan = self._prepare(DelayedFailingCache(values), "concurrent", 8)
        self.assertEqual(
            (Path(self.temp.name) / "sequential" / "plan.json").read_bytes(),
            (Path(self.temp.name) / "concurrent" / "plan.json").read_bytes(),
        )
        self.assertEqual(sequential, concurrent_plan)
        self.assertEqual(concurrent_plan["workers"], [])
        self.assertEqual(concurrent_plan["cache_stats"]["misses"], 0)

    def test_lookup_concurrency_is_bounded(self):
        cold = self._prepare(MemoryCache(), "cold", 1)
        values = self._results(cold["items"], cold)
        self.assertGreater(len(values), 2)
        for bound in (1, 2):
            cache = DelayedFailingCache(values, delay=0.02)
            self._prepare(cache, "bounded-%d" % bound, bound)
            self.assertEqual(cache.started, len(values))
            self.assertLessEqual(cache.max_in_flight, bound)

    def test_store_failure_cancels_queued_lookups(self):
        cold = self._prepare(MemoryCache(), "cold", 1)
        values = self._results(cold["items"], cold)
        failing = sorted(values)[0]
        cache = DelayedFailingCache(values, delay=0.01, failing=[failing])
        with self.assertRaises(CacheError):
            self._prepare(cache, "failing", 2)
        self.assertLessEqual(cache.started, cache.completed + 2)
        self.assertLess(cache.started, len(values))

    def test_invalid_entries_remain_misses_under_concurrency(self):
        cold = self._prepare(MemoryCache(), "cold", 1)
        values = self._results(cold["items"], cold)
        invalid = sorted(values)[0]
        cache = DelayedFailingCache(values, invalid=[invalid])
        plan = self._prepare(cache, "invalid", 8)
        self.assertEqual(plan["cache_stats"]["misses"], 1)
        self.assertNotIn(invalid, plan["hits"])
        self.assertEqual([invalid], plan["workers"][0]["keys"])

    def test_cache_concurrency_bounds_validated(self):
        for value in (0, -1, 65, True, 1.0, "8"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                self._prepare(MemoryCache(), "invalid-bound", value)

    def test_command_line_exposes_the_bound(self):
        arguments = parser().parse_args(
            [
                "prepare",
                "--repo",
                ".",
                "--head",
                "h",
                "--target",
                "t",
                "--identity",
                "i",
                "--profile",
                "p",
                "--rules",
                "r",
                "--cache",
                "c",
                "--output-dir",
                "o",
            ]
        )
        self.assertEqual(arguments.cache_concurrency, 8)
        self.assertEqual(
            parser()
            .parse_args(
                [
                    "compare",
                    "--repo",
                    ".",
                    "--head",
                    "h",
                    "--target",
                    "t",
                    "--identity",
                    "i",
                    "--profile",
                    "p",
                    "--rules",
                    "r",
                    "--cache",
                    "c",
                    "--output-dir",
                    "o",
                    "--cache-concurrency",
                    "3",
                ]
            )
            .cache_concurrency,
            3,
        )
