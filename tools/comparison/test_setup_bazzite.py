import hashlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import zipfile

from .setup_bazzite import atomic_publish, package_bundle, verify_assets
from .submit_bazzite import (
    coordinator_request,
    main,
    parent_invocation_id,
    runner_metadata,
)


class BazziteSetupTest(unittest.TestCase):
    def test_remote_coordinator_binds_custom_auth_name_to_server_credential(self):
        with tempfile.TemporaryDirectory() as temporary:
            config = Path(temporary) / "comparison.json"
            config.write_text(json.dumps({"api_key_env": "CUSTOM_BUILD_KEY"}))
            environment = {
                "BUILDBUDDY_API_KEY": "server-injected",
                "CUSTOM_BUILD_KEY": "unrelated-runner-value",
                "BUILDBUDDY_INVOCATION_ID": "01234567-89ab-cdef-0123-456789abcdef",
                "BUILDBUDDY_ARTIFACTS_DIRECTORY": temporary,
            }

            def coordinate(args):
                self.assertEqual(os.environ["CUSTOM_BUILD_KEY"], "server-injected")
                self.assertEqual(
                    args.pipeline_id, environment["BUILDBUDDY_INVOCATION_ID"]
                )
                return 0

            with (
                mock.patch.dict(os.environ, environment, clear=True),
                mock.patch(
                    "tools.comparison.submit_bazzite.coordinate", side_effect=coordinate
                ),
            ):
                self.assertEqual(
                    main(
                        [
                            "run-coordinator",
                            "--config",
                            str(config),
                            "--head",
                            "a" * 40,
                            "--branch",
                            "main",
                        ]
                    ),
                    0,
                )

    def test_runner_metadata_restricts_fields_and_handles_detached_checkout(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            artifacts = root / "artifacts/command-0"
            artifacts.mkdir(parents=True)
            rc = root / "buildbuddy.bazelrc"
            rc.write_text(
                "common --remote_header=x-buildbuddy-api-key=private\ncommon --build_metadata=BRANCH_NAME=feature/dogfood\ncommon --build_metadata=COMMIT_SHA="
                + "a" * 40
                + "\n"
            )
            self.assertEqual(
                runner_metadata("BRANCH_NAME", artifacts, {}), "feature/dogfood"
            )
            self.assertEqual(runner_metadata("COMMIT_SHA", artifacts, {}), "a" * 40)
            with self.assertRaisesRegex(ValueError, "unsupported"):
                runner_metadata("x-buildbuddy-api-key", artifacts, {})
            rc.write_text("common --build_metadata=COMMIT_SHA=private\n")
            with self.assertRaisesRegex(ValueError, "full Git SHA") as caught:
                runner_metadata("COMMIT_SHA", artifacts, {})
            self.assertNotIn("private", str(caught.exception))

    def test_parent_invocation_explicit_uuid_is_validated_and_takes_precedence(self):
        invocation = "01234567-89ab-cdef-0123-456789abcdef"
        self.assertEqual(
            parent_invocation_id(
                environment={"BUILDBUDDY_INVOCATION_ID": invocation.upper()}
            ),
            invocation,
        )
        for invalid in ("", "wrong", invocation + " --secret=hidden"):
            with (
                self.subTest(invalid=invalid),
                self.assertRaisesRegex(ValueError, "must be a UUID"),
            ):
                parent_invocation_id(environment={"BUILDBUDDY_INVOCATION_ID": invalid})

    def test_parent_invocation_reads_only_anchored_metadata_and_rejects_ambiguity(self):
        invocation = "01234567-89ab-cdef-0123-456789abcdef"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            artifacts = root / "artifacts/command-0"
            artifacts.mkdir(parents=True)
            rc = root / "buildbuddy.bazelrc"
            prefix = "common --remote_header=x-buildbuddy-api-key=private\n# common --build_metadata=PARENT_INVOCATION_ID=wrong\n"
            declaration = (
                "common --build_metadata=PARENT_INVOCATION_ID=" + invocation + "\n"
            )
            rc.write_text(prefix + declaration)
            self.assertEqual(
                parent_invocation_id(
                    environment={"BUILDBUDDY_ARTIFACTS_DIRECTORY": str(artifacts)}
                ),
                invocation,
            )
            self.assertEqual(parent_invocation_id(artifacts, {}), invocation)
            for contents in (
                prefix,
                prefix + declaration * 2,
                prefix
                + declaration
                + "common --build_metadata=PARENT_INVOCATION_ID=wrong\n",
            ):
                rc.write_text(contents)
                with self.assertRaisesRegex(
                    ValueError, "exactly one identity"
                ) as caught:
                    parent_invocation_id(artifacts, {})
                self.assertNotIn("private", str(caught.exception))
            rc.write_text(
                prefix + "common --build_metadata=PARENT_INVOCATION_ID=private\n"
            )
            with self.assertRaisesRegex(ValueError, "must be a UUID") as caught:
                parent_invocation_id(artifacts, {})
            self.assertNotIn("private", str(caught.exception))
            rc.unlink()
            with self.assertRaisesRegex(ValueError, "cannot read"):
                parent_invocation_id(artifacts, {})

    @unittest.skipUnless(os.name == "posix", "host paths require POSIX")
    def test_cpu_submission_pins_code_without_forwarding_local_credentials(self):
        config = {
            "execution_image": "none",
            "execution_bundle": "/var/lib/llm-cc/packages/code.zip",
            "execution_bundle_sha256": "a" * 64,
            "pool": "linux-amd64-rocm",
        }
        with mock.patch.dict(
            os.environ,
            {
                "BUILDBUDDY_API_KEY": "synthetic-secret",
                "GITHUB_TOKEN": "synthetic-github",
            },
            clear=True,
        ):
            request = coordinator_request(
                config,
                "/var/lib/llm-cc/comparison.json",
                "owner/repo",
                "b" * 40,
                "main",
            )
        self.assertEqual(request["commit_sha"], "b" * 40)
        self.assertEqual(request["platform_properties"]["Pool"], "linux-amd64-rocm")
        self.assertNotIn("skip_auto_checkout", request)
        self.assertNotIn("synthetic-secret", request["steps"][0]["run"])
        self.assertNotIn("gpu-resource", str(request["platform_properties"]))
        self.assertIn("tools.comparison.submit_bazzite", request["steps"][0]["run"])
        self.assertNotIn("remote_headers", request)
        self.assertNotIn("env", request)
        self.assertNotIn("synthetic-secret", str(request))
        self.assertNotIn("synthetic-github", str(request))

    def test_bundle_is_deterministic_and_excludes_unrelated_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package = root / "tools/comparison"
            package.mkdir(parents=True)
            (root / "tools/__init__.py").write_text("")
            for name in (
                "__init__.py",
                "__main__.py",
                "buildbuddy.py",
                "worker.py",
                "pipeline.py",
            ):
                (package / name).write_text("# source\n")
            (package / "dogfood-rules.json").write_text("{}")
            (package / "test_secret.py").write_text("private fixture")
            (package / "credentials.env").write_text("secret")
            first, checksum = package_bundle(root)
            os.utime(package / "worker.py", (1000000, 1000000))
            second, second_checksum = package_bundle(root)
            self.assertEqual(first, second)
            self.assertEqual(checksum, second_checksum)
            with zipfile.ZipFile(io.BytesIO(first)) as archive:
                self.assertNotIn("tools/comparison/test_secret.py", archive.namelist())
                self.assertNotIn("tools/comparison/credentials.env", archive.namelist())
                self.assertEqual(
                    archive.read("tools/comparison/worker.py"), b"# source\n"
                )
            if os.name == "posix":
                (package / "worker.py").unlink()
                (package / "worker.py").symlink_to(package / "pipeline.py")
                with self.assertRaisesRegex(ValueError, "regular file"):
                    package_bundle(root)

    def test_validated_assets_reject_modified_model_and_runtime(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            installed = root / "install"
            (installed / "bin").mkdir(parents=True)
            scorer = installed / "bin/llm-cc"
            scorer.write_bytes(b"scorer")
            scorer.chmod(0o755)
            model, runtime = root / "model", root / "runtime.so"
            model.write_bytes(b"model")
            runtime.write_bytes(b"runtime")

            def sha(path):
                return hashlib.sha256(path.read_bytes()).hexdigest()

            profile = {
                "build": {
                    "execution_image": "none",
                    "execution_host": {
                        "gpu_arch": "gfx1100",
                        "runtime_files": {str(runtime): sha(runtime)},
                    },
                    "installed_files": {"bin/llm-cc": sha(scorer)},
                    "model_bytes": 5,
                    "model_sha256": sha(model),
                }
            }
            self.assertEqual(
                verify_assets(profile, installed, model), (installed, model)
            )
            model.write_bytes(b"wrong")
            with self.assertRaisesRegex(ValueError, "model size or checksum"):
                verify_assets(profile, installed, model)
            model.write_bytes(b"model")
            runtime.write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "runtime file"):
                verify_assets(profile, installed, model)

    @unittest.skipUnless(os.name == "posix", "host publishing requires POSIX")
    def test_atomic_publish_replaces_complete_file_and_cleans_temporary(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "nested/config.json"
            atomic_publish(path, b"old")
            atomic_publish(path, b"new")
            self.assertEqual(path.read_bytes(), b"new")
            self.assertEqual(list(path.parent.iterdir()), [path])


if __name__ == "__main__":
    unittest.main()
