import json
import html
import os
import subprocess
import tempfile
import unittest
from unittest import mock
from pathlib import Path

from tools.comparison.cache import FilesystemStore, ResultCache
from tools.comparison.common import write_json
from tools.comparison.inventory import (
    GitError,
    _classify,
    inventory,
    merge_base,
    validate_rules,
)
from tools.comparison.pipeline import (
    _assemble,
    _code,
    _delta,
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
        self.assertIn("- `a.cc`: +2.5 LM-CC (+1 per token)", markdown)
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

    def test_zero_token_files_keep_raw_deltas(self):
        self.profile["max_file_bytes"] = 100
        out = Path(self.temp.name) / "zero"
        plan = prepare(
            self.repo, self.head, self.base, self.identity, self.profile, {},
            MemoryCache(), out / "probe", 1,
        )
        base = next(f for f in plan["inventories"]["base"] if f["path"] == "same.cc")
        _, report = self._scored(out, tokens=lambda key: 0 if key == base["key"] else 2)
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
                        "base": {"category": "runtime", "score": 1.0},
                        "head": {
                            "category": "runtime",
                            "score": 2.0,
                            "rank": 3,
                            "category_rank": 2,
                        },
                        "delta": 1.0,
                        "percent": 100.0,
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
                "base": {"category": "runtime", "score": 1.0},
                "head": {
                    "category": "runtime",
                    "score": 2.0 + index,
                    "rank": index + 1,
                    "category_rank": index + 1,
                },
                "delta": 1.0 + index,
                "percent": 100.0,
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
            head, sorted(head, key=lambda entry: (-entry["score"], entry["path"]))
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
        scorer = root / "fake-scorer.py"
        scorer.write_text("""#!/usr/bin/env python3
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
""")
        scorer.chmod(0o755)
        model = root / "model.gguf"
        model.write_bytes(b"model")
        profile = {
            "scoring": {"expected_configuration": {}},
            "build": {
                "installed_files": {
                    scorer.name: __import__("hashlib")
                    .sha256(scorer.read_bytes())
                    .hexdigest()
                },
                "source_commit": "4646123",
                "inference_abi": "test",
                "execution_image": "sha256:test",
                "container_environment_policy": "sanitized-v1",
                "model_sha256": __import__("hashlib").sha256(b"model").hexdigest(),
                "model_bytes": 5,
            },
            "max_file_bytes": 65536,
        }
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
