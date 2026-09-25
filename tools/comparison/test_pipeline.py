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
from tools.comparison.common import aggregate_report, write_json
from tools.comparison.fixtures import git, synthetic_scorer
from tools.comparison.inventory import (
    DEFAULT_RULES,
    GitError,
    _classify,
    inventory,
    merge_base,
    resolve_language,
    validate_rules,
    with_defaults,
)
from tools.comparison.pipeline import (
    REPOSITORY_RULES_PATH,
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
        self.assertEqual(rules, with_defaults({"tooling": ["copy.cc"]}))
        # Omitted keys keep llm-cc's built-in defaults, as local analysis does.
        self.assertEqual(rules["exclude"], DEFAULT_RULES["exclude"])
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
        self.assertEqual(missing, with_defaults({"tests": ["x"]}))

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
        self.assertEqual(rules, with_defaults({"tests": ["copy.cc"]}))
        self.assertEqual(source["path"], ".llm-cc/rules.json")
        git(self.repo, "rm", "-q", ".llm-cc/rules.json")
        git(self.repo, "commit", "-qm", "legacy rules only")
        legacy = git(self.repo, "rev-parse", "HEAD")
        rules, source = resolve_rules(self.repo, legacy, {}, REPOSITORY_RULES_PATH)
        self.assertEqual(rules, with_defaults({"tooling": ["copy.cc"]}))
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
        report = aggregate_report(
            Path(self.temp.name) / "empty-report",
            plan=Path(self.temp.name) / "empty-out/plan.json",
        )
        self.assertEqual(plan["workers"], [])
        self.assertEqual(report["status"], "complete")
        self.assertIsNone(report["comparisons"]["repository"]["score"]["head"])

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
