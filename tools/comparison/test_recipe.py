"""Offline end-to-end run of the CI recipe, plus template invariants.

A local repository, an in-memory GitHub and a filesystem store stand in for
the hosted services. Every GitLab job runs in its own directory holding only
the artifacts it declares, and the generated child-pipeline scripts run in a
real shell, so the recipe is exercised as CI would execute it.
"""

import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest
from unittest import mock

from .__main__ import main
from .cache import FilesystemStore
from .ci import gitlab_child, gitlab_skipped, load_config, write_yaml
from .common import pipeline_prefix, read_json, write_json
from .fixtures import FakeGitHub, commit_files, git, synthetic_scorer
from .publish import marker_key

ROOT = Path(__file__).resolve().parents[2]
RECIPE = Path(__file__).resolve().parent / "recipe"
REPOSITORY = "owner/repo"
COORDINATOR = "registry.example/llm-cc-coordinator@sha256:" + "2" * 64
PLAN = "comparison/prep/plan.json"
# The issue's documented warm-path budget; the local run is far below it.
WARM_BUDGET_SECONDS = 60


class Run:
    def __init__(self, pipeline_id, ordinal, head, root):
        self.pipeline_id, self.ordinal, self.head = pipeline_id, ordinal, head
        self.directory = root / ("pipeline-" + pipeline_id)
        self.prepare = self.directory / "comparison-prepare"
        self.identity = self.prepare / "comparison" / "identity.json"
        self.skipped = False
        self.child = {}
        self.jobs = {}


class RecipeTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        git(self.repo, "init", "-q", "-b", "main")
        self.github = FakeGitHub(REPOSITORY)
        scorer, model, profile = synthetic_scorer(
            self.root / "scorer", max_file_bytes=256
        )
        self.profile = self.root / "profile.json"
        write_json(self.profile, profile)
        self.rules = self.root / "rules.json"
        write_json(self.rules, {"exclude": ["vendor/**"], "tests": ["tests/**"]})
        self.config = self.root / "ci.json"
        write_json(
            self.config,
            {
                "schema_version": 1,
                "images": {"coordinator": COORDINATOR},
                "scorer": {
                    "executable": str(scorer),
                    "installed_root": str(scorer.parent),
                    "model": str(model),
                },
                "gitlab": {"cpu_tags": ["cpu"], "gpu_tags": ["gpu"]},
            },
        )
        self.store = self.root / "store"
        self.environment = os.environ | {
            "PYTHONPATH": str(ROOT),
            "PYTHONDONTWRITEBYTECODE": "1",
        }
        self.ordinal = 0

    def cli(self, *arguments):
        output, errors = io.StringIO(), io.StringIO()
        with (
            mock.patch("tools.comparison.__main__.GitHub", return_value=self.github),
            contextlib.redirect_stdout(output),
            contextlib.redirect_stderr(errors),
        ):
            status = main([str(argument) for argument in arguments])
        self.assertEqual(status, 0, errors.getvalue())
        return output.getvalue()

    def push(self, branch, files, message):
        if git(self.repo, "branch", "--show-current") != branch:
            git(self.repo, "checkout", "-q", "-B", branch)
        sha = commit_files(self.repo, files, message)
        self.github.branches[branch] = sha
        return sha

    def download(self, job_directory, source_directory, paths):
        for path in paths:
            source = source_directory / path
            if source.exists():
                shutil.copytree(source, job_directory / path, dirs_exist_ok=True)

    def run_job(self, run, name, job):
        """Run one generated child job the way a GitLab runner would."""
        directory = run.directory / "child" / name
        directory.mkdir(parents=True)
        # Download worker artifacts before the parent's, so preparation files
        # would clobber completed results if the two ever overlapped.
        for need in sorted(job["needs"], key=lambda need: "pipeline" in need):
            if "pipeline" in need:
                self.download(directory, run.prepare, ["comparison/"])
            else:
                upstream = run.child[need["job"]]
                self.download(
                    directory,
                    run.directory / "child" / need["job"],
                    upstream["artifacts"]["paths"],
                )
        completed = subprocess.run(
            ["sh", "-ec", "\n".join(job["script"])],
            cwd=directory,
            env=self.environment,
            capture_output=True,
            text=True,
        )
        run.jobs[name] = completed
        return completed

    def pipeline(self, branch, head):
        """The parent pipeline and its generated child, short of publication."""
        self.ordinal += 1
        run = Run(str(9000 + self.ordinal), self.ordinal, head, self.root)
        output = run.prepare / "comparison"
        output.mkdir(parents=True)
        self.cli(
            "discover",
            "--repository",
            REPOSITORY,
            "--head",
            head,
            "--branch",
            branch,
            "--default-branch",
            "main",
            "--pipeline-id",
            run.pipeline_id,
            "--output",
            run.identity,
            "--dotenv",
            output / "discover.env",
        )
        variables = dict(
            line.split("=", 1)
            for line in (output / "discover.env").read_text().splitlines()
        )
        config = load_config(self.config)
        if variables["COMPARISON_SKIP"] == "1":
            run.skipped = True
            run.child = gitlab_skipped(config, "no open pull request")
            return run
        self.cli(
            "prepare",
            "--repo",
            self.repo,
            "--head",
            head,
            "--target",
            variables["COMPARISON_TARGET_SHA"],
            "--identity",
            run.identity,
            "--profile",
            self.profile,
            "--rules",
            self.rules,
            "--cache",
            self.store,
            "--config",
            self.config,
            "--output-dir",
            output / "prep",
        )
        write_yaml(
            output / "child.yml",
            gitlab_child(read_json(run.prepare / PLAN), PLAN, config, str(self.store)),
        )
        run.child = json.loads(
            (output / "child.yml").read_text(encoding="utf-8").split("\n", 1)[1]
        )
        for name, job in run.child.items():
            if name.startswith("comparison-worker-"):
                self.run_job(run, name, job)
        aggregate = self.run_job(
            run, "comparison-aggregate", run.child["comparison-aggregate"]
        )
        self.assertEqual(aggregate.returncode, 0, aggregate.stderr)
        return run

    def publish(self, run):
        return (
            self.cli(
                "publish",
                "--cache",
                self.store,
                "--repository",
                REPOSITORY,
                "--pipeline-id",
                run.pipeline_id,
                "--head",
                run.head,
                "--ordinal",
                run.ordinal,
                "--identity",
                run.identity,
                "--comment-author",
                self.github.login,
            )
            .split(":")[1]
            .strip()
        )

    def report(self, run):
        prefix = pipeline_prefix(
            {"repository": REPOSITORY, "pipeline_id": run.pipeline_id}
        )
        return json.loads(FilesystemStore(self.store).get(prefix + "report.json"))

    def workers(self, run):
        return [name for name in run.child if name.startswith("comparison-worker-")]

    def test_configured_capacity_alone_bounds_the_plan(self):
        # Only the configuration lowers capacity; COMPARISON_MAX_WORKERS keeps
        # its default, and child generation must still accept the plan.
        config = read_json(self.config)
        write_json(self.config, dict(config, max_workers=1))
        base = self.push(
            "main",
            {"src/%s.cc" % name: "int %s;\n" % name for name in "abcdef"},
            "six distinct contents",
        )
        run = self.pipeline("main", base)
        self.assertEqual(self.workers(run), ["comparison-worker-0"])
        self.assertEqual(self.report(run)["status"], "complete")

    def test_recipe_from_baseline_to_retargeted_pull_request(self):
        engine = "int engine() { return 1; }\n"
        base = self.push(
            "main",
            {
                "src/engine.cc": engine,
                "src/copy.cc": engine,
                "src/util.cc": "int util;\n",
                "include/api.h": "int engine();\n",
                "tests/engine_test.cc": "int test;\n",
                "vendor/lib.cc": "int vendored;\n",
                "README.md": "docs\n",
            },
            "baseline",
        )

        # A default-branch push seeds the cache and stores, but never comments.
        baseline = self.pipeline("main", base)
        self.assertEqual(self.report(baseline)["status"], "complete")
        self.assertGreater(len(self.workers(baseline)), 0)
        self.assertEqual(self.publish(baseline), "stored")

        # A branch without a pull request schedules no scoring at all.
        head = self.push(
            "feature",
            {
                "src/engine.cc": "int engine() { return 2; }\n",
                "src/util.cc": None,
                "tests/util_test.cc": "int util;\n",
                "src/new.py": "value = 1\n",
            },
            "change engine, move util into tests, add a module",
        )
        skipped = self.pipeline("feature", head)
        self.assertTrue(skipped.skipped)
        self.assertEqual(list(skipped.child), ["comparison-skipped"])

        # Cold PR run: only the two new contents reach GPU workers.
        pull = self.github.open_pull(1, "feature", head)
        cold = self.pipeline("feature", head)
        report = self.report(cold)
        self.assertEqual(report["status"], "complete", report["errors"])
        self.assertEqual(report["cache_stats"]["misses"], 2)
        self.assertEqual(len(self.workers(cold)), 2)
        self.assertEqual(
            report["change_counts"],
            {"additions": 1, "deletions": 0, "renames": 1, "category_moves": 1},
        )
        base_paths = {row["path"]: row for row in report["inventories"]["base"]}
        # Duplicate contents share one key yet count once per path.
        self.assertEqual(
            base_paths["src/engine.cc"]["key"], base_paths["src/copy.cc"]["key"]
        )
        self.assertEqual(report["sides"]["base"]["totals"]["measured_paths"], 5)
        self.assertTrue(base_paths["include/api.h"]["scorable"])
        self.assertEqual(base_paths["vendor/lib.cc"]["reason"], "excluded")
        self.assertEqual(base_paths["README.md"]["reason"], "unsupported")
        self.assertEqual(self.publish(cold), "published")
        [comment] = self.github.pull_comments(1)
        self.assertIn(
            "pipeline=%s ordinal=%d" % (cold.pipeline_id, cold.ordinal), comment["body"]
        )

        # Warm second push: a rename and a duplicate need no GPU and update
        # the same comment.
        started = time.monotonic()
        head = self.push(
            "feature",
            {
                "src/new.py": None,
                "src/renamed.py": "value = 1\n",
                "src/engine_copy.cc": "int engine() { return 2; }\n",
            },
            "rename and duplicate",
        )
        pull["head"]["sha"] = head
        warm = self.pipeline("feature", head)
        self.assertEqual(self.publish(warm), "published")
        elapsed = time.monotonic() - started
        self.assertEqual(self.workers(warm), [])
        self.assertEqual(sorted(warm.child), ["comparison-aggregate", "stages"])
        self.assertEqual(self.report(warm)["cache_stats"]["misses"], 0)
        self.assertLess(elapsed, WARM_BUDGET_SECONDS)
        [updated] = self.github.pull_comments(1)
        self.assertEqual(updated["id"], comment["id"])
        self.assertIn("pipeline=%s" % warm.pipeline_id, updated["body"])

        # The delayed older pipeline cannot overwrite the newer comment.
        self.assertEqual(self.publish(cold), "stale")
        self.assertEqual(self.github.pull_comments(1), [updated])

        # An oversized file is visible as unmeasured coverage, and a PR
        # retargeted before publication is suppressed.
        head = self.push("feature", {"src/big.py": "x = 1\n" * 100}, "oversized")
        pull["head"]["sha"] = head
        retargeted = self.pipeline("feature", head)
        report = self.report(retargeted)
        self.assertEqual(report["status"], "incomplete")
        self.assertEqual(self.workers(retargeted), [])
        head_paths = {row["path"]: row for row in report["inventories"]["head"]}
        self.assertEqual(head_paths["src/big.py"]["reason"], "oversized")
        self.assertIsNone(report["comparisons"]["runtime"]["raw_llm_cc"]["head"])
        self.github.branches["release"] = base
        pull["base"]["ref"] = "release"
        self.assertEqual(self.publish(retargeted), "suppressed")
        self.assertEqual(self.github.pull_comments(1), [updated])
        marker = json.loads(FilesystemStore(self.store).get(marker_key(REPOSITORY, 1)))
        self.assertEqual(
            (marker["ordinal"], marker["state"], marker["comment_id"]),
            (retargeted.ordinal, "suppressed", comment["id"]),
        )


class TemplateTest(unittest.TestCase):
    def read(self, *parts):
        return RECIPE.joinpath(*parts).read_text(encoding="utf-8")

    def test_images_pin_the_model_and_keep_credentials_out_of_layers(self):
        model = self.read("images", "model.Containerfile")
        self.assertIn("ARG MODEL_SHA256", model)
        self.assertIn("ARG MODEL_BYTES", model)
        self.assertIn("sha256sum -c", model)
        self.assertIn("FROM scratch", model)
        self.assertIn("/models/model.gguf", model)
        scorer = self.read("images", "scorer.Containerfile")
        self.assertIn("ARG MODEL_IMAGE", scorer)
        self.assertIn("@sha256:[0-9a-f]{64}", scorer)
        self.assertIn("--mount=type=secret,id=bazelrc", scorer)
        self.assertIn("--//:source_commit=", scorer)
        self.assertIn("--//:source_version=", scorer)
        self.assertIn("compute_86", scorer)
        self.assertIn("profile generate", scorer)
        self.assertRegex(scorer, r"(?m)^USER [1-9][0-9]*")
        self.assertNotRegex(scorer, r"(?i)ARG [A-Z_]*(TOKEN|SECRET|PASSWORD|API_KEY)")
        coordinator = self.read("images", "coordinator.Containerfile")
        self.assertIn("git", coordinator)
        self.assertIn("boto3", coordinator)
        self.assertNotIn("gguf", coordinator.lower())
        self.assertNotIn("MODEL_IMAGE", coordinator)
        self.assertRegex(coordinator, r"(?m)^USER [1-9][0-9]*")
        # Jobs run from a checkout; its own tools/ must not replace the pinned one.
        for image in (scorer, coordinator):
            self.assertRegex(image, r"(?m)^\s+PYTHONSAFEPATH=1 \\$")
            self.assertIn("sys.version_info < (3, 11)", image)

    def image_build(self, skopeo):
        """A pinned llm-cc checkout, a fake engine and registry, and build.sh's
        environment; the engine pushes every image as digest 0."""
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        source = root / "llm-cc"
        source.mkdir()
        git(source, "init", "-q", "-b", "main")
        commit = commit_files(
            source,
            {"tools/__init__.py": "", "tools/comparison/__init__.py": ""},
            "pinned",
        )
        tools = root / "bin"
        tools.mkdir()
        (tools / "skopeo").write_text(skopeo)
        (tools / "podman").write_text(
            '#!/bin/sh\necho "$*" >> "$ENGINE_LOG"\ncase "$1" in\n'
            "  push) printf 'sha256:%064d' 0 > \"$3\" ;;\n"
            "  run) echo '{}' ;;\nesac\n"
        )
        for tool in tools.iterdir():
            tool.chmod(0o755)
        environment = {
            "PATH": str(tools) + os.pathsep + os.environ["PATH"],
            "HOME": str(root),
            "ENGINE_LOG": str(root / "engine.log"),
            "REGISTRY": "registry.example/llm-cc",
            "LLM_CC_COMMIT": commit,
            "LLM_CC_VERSION": "0.0.0",
            "LLM_CC_SOURCE": str(source),
            "MODEL_URL": "https://models.example/model.gguf",
            "MODEL_SHA256": "1" * 64,
            "MODEL_BYTES": "1",
            "BAZELISK_URL": "https://bazelisk.example/bazelisk",
            "BAZELISK_SHA256": "3" * 64,
        }
        return source, environment

    def build_images(self, source, environment):
        return subprocess.run(
            ["sh", str(RECIPE / "images" / "build.sh")],
            cwd=source,
            env=environment,
            capture_output=True,
            text=True,
        )

    @unittest.skipUnless(shutil.which("sha256sum"), "build.sh needs sha256sum")
    def test_build_script_reruns_in_the_checkout_and_forwards_pins(self):
        # An empty registry, so every run builds, pushes and generates.
        source, environment = self.image_build("#!/bin/sh\nexit 1\n")
        fetch = "registry.example/alpine@sha256:" + "4" * 64
        environment["FETCH_IMAGE"] = fetch
        # The default OUTPUT lands inside the checkout; a rerun must still
        # pass the clean-source check.
        for _ in range(2):
            completed = self.build_images(source, environment)
            self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn(
            "COORDINATOR_IMAGE=registry.example/llm-cc/coordinator@sha256:" + "0" * 64,
            (source / "comparison" / "images.env").read_text(),
        )
        models = [
            line
            for line in Path(environment["ENGINE_LOG"]).read_text().splitlines()
            if line.startswith("build ") and "model.Containerfile" in line
        ]
        self.assertEqual(len(models), 2)
        for line in models:
            self.assertIn("--build-arg FETCH_IMAGE=" + fetch, line)
        # Only the outputs are exempt: an edited source still refuses to build.
        (source / "tools" / "__init__.py").write_text("# edited\n")
        completed = self.build_images(source, environment)
        self.assertEqual(completed.returncode, 1)
        self.assertIn("is not a clean checkout", completed.stderr)

    @unittest.skipUnless(shutil.which("sha256sum"), "build.sh needs sha256sum")
    def test_build_script_exempts_only_the_files_it_writes(self):
        # An OUTPUT holding tracked source: edits to that source still count.
        source, environment = self.image_build("#!/bin/sh\nexit 1\n")
        environment["OUTPUT"] = str(source / "tools" / "comparison")
        completed = self.build_images(source, environment)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        (source / "tools" / "comparison" / "__init__.py").write_text("# edited\n")
        completed = self.build_images(source, environment)
        self.assertEqual(completed.returncode, 1)
        self.assertIn("is not a clean checkout", completed.stderr)
        # Nor may an output replace a tracked file.
        source, environment = self.image_build("#!/bin/sh\nexit 1\n")
        environment["LLM_CC_COMMIT"] = commit_files(
            source, {"tools/profile.json": "{}\n"}, "a tracked profile"
        )
        environment["OUTPUT"] = str(source / "tools")
        completed = self.build_images(source, environment)
        self.assertEqual(completed.returncode, 1)
        self.assertIn("overwrite tracked source tools/profile.json", completed.stderr)

    @unittest.skipUnless(shutil.which("sha256sum"), "build.sh needs sha256sum")
    def test_build_script_reuses_images_only_for_the_same_inputs(self):
        # Every tag exists as digest 5, labelled with the pinned inputs and the
        # tag it was built for, except for the model size, which the registry
        # reports as REGISTRY_BYTES, and a tag moved to another image.
        source, environment = self.image_build(
            '#!/bin/sh\necho "$4" >> "$REGISTRY_LOG"\ncase "$3" in\n'
            '  \'{{.Digest}}\') echo "${4##*:}" > "$REGISTRY_LOG.tag"\n'
            "    printf 'sha256:%064d\\n' 5 ;;\n"
            '  *model.sha256*) echo "$MODEL_SHA256" ;;\n'
            '  *model.bytes*) echo "$REGISTRY_BYTES" ;;\n'
            '  *revision*) echo "$LLM_CC_COMMIT" ;;\n'
            '  *input-key*) if [ -n "${REGISTRY_MOVED:-}" ]; then echo moved;\n'
            '    else cat "$REGISTRY_LOG.tag"; fi ;;\nesac\n'
        )
        registry = Path(environment["HOME"]) / "registry.log"
        engine = Path(environment["ENGINE_LOG"])
        environment |= {
            "REGISTRY_LOG": str(registry),
            "REGISTRY_BYTES": "1",
            "TRUST_REGISTRY": "1",
        }

        def scorer_tags():
            return [
                line.rsplit(":", 1)[1]
                for line in registry.read_text().splitlines()
                if "/scorer:" in line
            ]

        completed = self.build_images(source, environment)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertFalse(
            [
                line
                for line in engine.read_text().splitlines()
                if line.startswith("build ")
            ]
        )
        # The profile records MODEL_BYTES, so an image recording another size
        # is rebuilt, and the build's own size check decides.
        environment["REGISTRY_BYTES"] = "2"
        completed = self.build_images(source, environment)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("io.llm-cc.model.bytes is '2', expected '1'", completed.stderr)
        self.assertIn("model.Containerfile", engine.read_text())
        # A new Bazelisk pin is a new scorer, not a reused one.
        environment |= {"REGISTRY_BYTES": "1", "BAZELISK_SHA256": "6" * 64}
        completed = self.build_images(source, environment)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        first, _, changed = scorer_tags()
        self.assertNotEqual(first, changed)
        # A tag moved to another signed image of the same revision is rebuilt,
        # and the rebuilt images record the tags they were built for.
        engine.write_text("")
        environment["REGISTRY_MOVED"] = "1"
        completed = self.build_images(source, environment)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("io.llm-cc.input-key is 'moved'", completed.stderr)
        builds = [
            line
            for line in engine.read_text().splitlines()
            if line.startswith("build ")
        ]
        self.assertEqual(len(builds), 2)
        for line, containerfile in zip(builds, ("scorer", "coordinator")):
            self.assertIn(containerfile + ".Containerfile", line)
            tag = line.split("--tag ", 1)[1].split(" ", 1)[0].rsplit(":", 1)[1]
            self.assertIn("--label io.llm-cc.input-key=" + tag, line)

    def test_gitlab_parent_serializes_publication_and_always_reports(self):
        parent = self.read("gitlab", ".gitlab-ci.yml")
        for required in (
            "tools.comparison discover",
            "tools.comparison prepare",
            "tools.comparison ci gitlab-child",
            "strategy: depend",
            "PARENT_PIPELINE_ID: $CI_PIPELINE_ID",
            "resource_group: comparison-$CI_COMMIT_REF_SLUG",
            "when: always",
            "interruptible: true",
            '--ordinal "$CI_PIPELINE_IID"',
            "COMPARISON_SKIP",
            "GITHUB_COMMENT_TOKEN",
            "GITHUB_READ_TOKEN",
            "tools.comparison publish",
            "dotenv:",
        ):
            with self.subTest(required=required):
                self.assertIn(required, parent)
        self.assertIn("@sha256:", parent)
        # The comment credential never reaches discovery or preparation.
        prepare = parent.split("comparison-prepare:", 1)[1].split("\ncomparison-", 1)[0]
        self.assertNotIn("GITHUB_COMMENT_TOKEN", prepare)

    def test_github_workflow_gates_gpus_and_scopes_permissions(self):
        workflow = self.read("github", "comparison.yml")
        for required in (
            "concurrency:",
            "permissions: {}",
            "ci github-matrix",
            "needs.prepare.outputs.has_workers == 'true'",
            "fromJSON(needs.prepare.outputs.matrix)",
            "self-hosted",
            "--gpus",
            # Aggregation and publication survive failed workers, not cancellation.
            "if: ${{ !cancelled() && needs.prepare.outputs.skip == '0' }}",
            "if: ${{ !cancelled() && needs.prepare.outputs.skip != '1' }}",
            "pull-requests: write",
            "contents: read",
            "tools.comparison publish",
            "github-actions[bot]",
            '--ordinal "$GITHUB_RUN_ID"',
        ):
            with self.subTest(required=required):
                self.assertIn(required, workflow)
        publish = workflow.split("\n  publish:", 1)[1]
        self.assertIn("pull-requests: write", publish)
        before = workflow.split("\n  publish:", 1)[0]
        self.assertNotIn("pull-requests: write", before)

    def test_example_configuration_is_valid(self):
        config = load_config(RECIPE / "config" / "ci.example.json")
        self.assertEqual(config["max_workers"], 4)
        self.assertEqual(config["worker_timeout"], "120m")
        from .inventory import validate_rules

        validate_rules(json.loads(self.read("config", "rules.example.json")))


if __name__ == "__main__":
    unittest.main()
