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
from tools.comparison.inventory import GitError, _classify, inventory, merge_base
from tools.comparison.pipeline import (
    _delta,
    _render,
    _safe,
    aggregate,
    compare,
    prepare,
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

    def test_report_escaped_paths_render_as_text_instead_of_literal_entities(self):
        path = "src/#35/@team/[file]`name`~~$x$.cc"
        comparison = {
            "score": _delta(1.0, 2.0),
            "raw_llm_cc": _delta(1.0, 2.0),
            "tokens": _delta(1, 1),
            "coverage": {"base": 1.0, "head": 1.0},
        }
        markdown = _render(
            {
                "status": "complete",
                "comparisons": {
                    name: comparison
                    for name in ("runtime", "tests", "tooling", "repository")
                },
                "cache_stats": {"hits": 1, "misses": 0},
                "leading_regressions": [{"path": path, "change": 1.0}],
                "leading_improvements": [],
                "errors": [],
            }
        )
        path_line = next(
            line for line in markdown.splitlines() if line.startswith("- ")
        )
        self.assertNotIn("`", path_line)
        self.assertNotRegex(path_line, r"(?<!&)#\d")
        self.assertNotIn("@team", path_line)
        self.assertNotIn("[file]", path_line)
        self.assertNotIn("~~", path_line)
        self.assertNotIn("$x$", path_line)
        self.assertEqual(
            html.unescape(path_line), "- " + path.replace("@", "@\u200b") + ": +1"
        )

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
            "odd&#92;u000aname&#46;py", (out / "report/report.md").read_text()
        )
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
