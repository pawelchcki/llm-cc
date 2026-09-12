"""Pinned ROCm and CUDA contracts are distinct and reject mutable identities."""

import copy
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest

from .common import CONTAINER_ENVIRONMENT_POLICY, digest
from .profile import dogfood_profile, SOURCE_COMMIT


class ProfileTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / "bin").mkdir()
        (self.root / "bin/llm-cc").write_bytes(b"pinned scorer")
        backend_dir = self.root / "lib/llm-cc/backends/linux-x86_64"
        backend_dir.mkdir(parents=True)
        for backend in ("cuda", "rocm"):
            bundle = (backend + " bundle").encode()
            (backend_dir / f"{backend}.bundle").write_bytes(bundle)
            (backend_dir / f"{backend}.manifest.json").write_text(
                json.dumps(
                    {
                        "git_sha": SOURCE_COMMIT,
                        "sha256": hashlib.sha256(bundle).hexdigest(),
                        "size": len(bundle),
                    }
                )
            )
        self.image = "registry.example/scorer@sha256:" + "a" * 64
        self.runtime = str(self.root / "host-runtime.so")
        self.host = {
            "gpu_pci_address": "0000:03:00.0",
            "gpu_arch": "gfx1100",
            "gpu_vendor": "amd",
            "gpu_vram_bytes_min": 25753026560,
            "resource_id": "bazzite-radeon-0",
            "runtime_files": {self.runtime: "b" * 64},
        }

    def rocm(self, **kwargs):
        return dogfood_profile(
            self.root, "none", backend="rocm", execution_host=self.host, **kwargs
        )

    def test_cuda_default_contract_preserved(self):
        profile = dogfood_profile(self.root, self.image)
        config = profile["scoring"]["expected_configuration"]
        self.assertEqual(config["backend"], "cuda/gpu-layers=-1")
        self.assertEqual(config["context"], 131072)
        self.assertEqual(config["batch_size"], 256)
        self.assertEqual(config["kv_cache_type"], "q8_0")
        self.assertEqual(profile["max_file_bytes"], 65536)
        self.assertNotIn("execution_host", profile["build"])
        self.assertEqual(
            profile["build"]["container_environment_policy"],
            CONTAINER_ENVIRONMENT_POLICY,
        )
        old_profile = copy.deepcopy(profile)
        del old_profile["build"]["container_environment_policy"]
        self.assertNotEqual(digest(profile), digest(old_profile))
        self.assertEqual(
            profile["build"]["installed_files"]["bin/llm-cc"],
            hashlib.sha256(b"pinned scorer").hexdigest(),
        )

    def test_rocm_contract_and_explicit_settings(self):
        profile = self.rocm()
        config = profile["scoring"]["expected_configuration"]
        self.assertEqual(config["backend"], "rocm/gpu-layers=-1")
        self.assertEqual(config["context"], 32768)
        self.assertEqual(profile["build"]["execution_image"], "none")
        self.assertEqual(profile["build"]["execution_host"], self.host)
        self.assertNotIn("container_environment_policy", profile["build"])
        custom = self.rocm(context=65536, batch_size=64, kv_cache_type="q4_0")
        config = custom["scoring"]["expected_configuration"]
        self.assertEqual(config["context"], 65536)
        self.assertEqual(config["batch_size"], 64)
        self.assertEqual(config["kv_cache_type"], "q4_0")
        argv = custom["scoring"]["argv"]
        self.assertEqual(argv[argv.index("--context") + 1], "65536")
        self.assertEqual(argv[argv.index("--batch-size") + 1], "64")
        self.assertEqual(argv[argv.index("--kv-cache-type") + 1], "q4_0")
        self.host["runtime_files"][self.runtime] = "c" * 64
        self.assertEqual(
            profile["build"]["execution_host"]["runtime_files"][self.runtime],
            "b" * 64,
        )

    def test_every_inference_identity_change_invalidates_fingerprint(self):
        original = self.rocm()
        variants = [
            dogfood_profile(self.root, self.image),
            self.rocm(context=65536),
            self.rocm(batch_size=64),
            self.rocm(kv_cache_type="q4_0"),
        ]
        for field, value in (
            ("gpu_arch", "gfx1101"),
            ("runtime_files", {self.runtime: "c" * 64}),
        ):
            changed = copy.deepcopy(original)
            changed["build"]["execution_host"][field] = value
            variants.append(changed)
        for changed in variants:
            self.assertNotEqual(digest(original), digest(changed))

    def test_mutable_or_incomplete_execution_identity_rejected(self):
        for image, host in (
            ("registry.example/scorer:latest", None),
            ("none", None),
            (self.image, self.host),
        ):
            with self.subTest(image=image), self.assertRaises(ValueError):
                dogfood_profile(self.root, image, backend="rocm", execution_host=host)
        for field, value in (
            ("gpu_pci_address", "03:00.0"),
            ("gpu_arch", "navi31"),
            ("gpu_vendor", "nvidia"),
            ("gpu_vram_bytes_min", True),
            ("resource_id", "../shared"),
            ("resource_id", "Bazzite"),
            ("resource_id", "bazzite.radeon"),
            ("resource_id", "b" * 65),
            ("runtime_files", {}),
            ("runtime_files", {"relative/libc.so.6": "b" * 64}),
            ("runtime_files", {str(self.root / ".." / "host-runtime.so"): "b" * 64}),
            ("runtime_files", {self.runtime: "latest"}),
        ):
            host = {**self.host, field: value}
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                dogfood_profile(self.root, "none", backend="rocm", execution_host=host)

    def test_invalid_scoring_settings_rejected(self):
        for kwargs in (
            {"context": 0},
            {"context": True},
            {"context": 2**32},
            {"batch_size": -1},
            {"batch_size": 1.5},
            {"kv_cache_type": "auto"},
            {"max_file_bytes": 0},
        ):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                self.rocm(**kwargs)

    def test_unpinned_backend_rejected(self):
        manifest = self.root / "lib/llm-cc/backends/linux-x86_64/rocm.manifest.json"
        manifest.write_text(json.dumps({"git_sha": "main"}))
        with self.assertRaisesRegex(ValueError, SOURCE_COMMIT):
            self.rocm()

    @unittest.skipUnless(os.name == "posix", "symlink creation requires POSIX")
    def test_installed_symlink_rejected(self):
        (self.root / "bin/link").symlink_to("llm-cc")
        with self.assertRaisesRegex(ValueError, "symlinks"):
            self.rocm()

    def test_manifest_must_describe_installed_bundle(self):
        manifest = self.root / "lib/llm-cc/backends/linux-x86_64/rocm.manifest.json"
        original = json.loads(manifest.read_text())
        for key, value in (("sha256", "d" * 64), ("size", 0), ("size", True)):
            manifest.write_text(json.dumps({**original, key: value}))
            with (
                self.subTest(key=key, value=value),
                self.assertRaisesRegex(ValueError, "manifest checksum or size"),
            ):
                self.rocm()
        manifest.write_text(json.dumps(original))
        manifest.with_name("rocm.bundle").unlink()
        with self.assertRaisesRegex(ValueError, "manifest checksum or size"):
            self.rocm()


if __name__ == "__main__":
    unittest.main()
