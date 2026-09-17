"""Profiles derive their identity from the installed scorer and stay pinned."""

import copy
import dataclasses
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
import unittest.mock

from . import profile as profile_module
from .common import CONTAINER_ENVIRONMENT_POLICY, digest
from .profile import (
    build_profile,
    dogfood_profile,
    inspect_installation,
    main,
    ModelSpec,
    ScoringSettings,
    INFERENCE_ABI,
    PINNED_ANALYSIS_VERSION,
    SOURCE_COMMIT,
)

LLAMA_CPP_COMMIT = "c589f0ed10c643678c4707dd160c21ac7633ebc0"
SANITIZED_ENVIRONMENT = {
    "PATH",
    "LANG",
    "HOME",
    "LLM_CC_ENTROPY_CACHE_DIR",
    "LLM_CC_CACHE_DIR",
    "LLM_CC_RUNTIME_DIR",
}

# Frozen copy of the contract the shared result cache was populated with.  Any
# change here invalidates every cached dogfood result.
FROZEN_ARGV = [
    "--backend",
    "rocm",
    "--gpu-layers",
    "-1",
    "--context",
    "32768",
    "--batch-size",
    "256",
    "--flash-attn",
    "on",
    "--kv-cache-type",
    "q8_0",
    "--kv-offload",
    "on",
    "--entropy-reduction",
    "device",
    "--hierarchy",
    "structural",
    "--tau",
    "0.67",
    "--alpha",
    "0.8",
    "--score",
    "lmcc",
    "--progress",
    "always",
    "--hotspots",
    "0",
]
FROZEN_EXPECTED = {
    "analysis_version": 2,
    "hierarchy_mode": "structural",
    "include_headers": True,
    "no_ignore": True,
    "no_download": True,
    "progress": "always",
    "context": 32768,
    "batch_size": 256,
    "entropy_reduction": "device",
    "effective_entropy_reducer": "device",
    "flash_attn": "on",
    "effective_flash_attn": "on",
    "kv_cache_type": "q8_0",
    "effective_kv_cache_type": "q8_0",
    "kv_offload": "on",
    "effective_kv_offload": "on",
    "backend_diagnostics": False,
    "score_mode": "lmcc",
    "tau_rule": "absolute",
    "tau": 0.67,
    "tau_percentile": None,
    "alpha": 0.8,
    "hotspots": 0,
    "backend": "rocm/gpu-layers=-1",
    "gpu_layers": -1,
    "inference_abi": INFERENCE_ABI,
}

SCORER_SOURCE = '''\
import json
import os
import sys

SUPPORT = {support!r}


def marker(name):
    entropy = os.environ.get("LLM_CC_ENTROPY_CACHE_DIR", "")
    record = {{
        "argv": sys.argv[1:],
        "environment": sorted(os.environ),
        "cwd": os.getcwd(),
        "home": os.environ.get("HOME"),
        "entropy": entropy,
        "runtime": os.environ.get("LLM_CC_RUNTIME_DIR"),
    }}
    with open(os.path.join(SUPPORT, name + "-call.json"), "w") as stream:
        json.dump(record, stream)
    if entropy and os.path.isdir(entropy):
        with open(os.path.join(entropy, "scorer-marker"), "w") as stream:
            stream.write("ok")


def emit(name):
    with open(os.path.join(SUPPORT, name)) as stream:
        sys.stdout.write(stream.read())


arguments = sys.argv[1:]
if arguments == ["--version"]:
    marker("version")
    emit("version.txt")
    sys.exit(int(open(os.path.join(SUPPORT, "version-exit.txt")).read()))
if arguments[:2] == ["cache", "status"] and "--format" in arguments:
    marker("status")
    emit("status.json")
    sys.exit(0)
sys.exit(2)
'''


# Settings the scorer rejects in combination, so varying one field at a time
# still produces a profile it would accept.
LEGAL_COMPANIONS = {
    "gpu_layers": {"entropy_reduction": "host"},
    "flash_attn": {"kv_cache_type": "f16"},
}


class ProfileTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        support = tempfile.TemporaryDirectory()
        self.addCleanup(support.cleanup)
        self.support = Path(support.name)
        self.version = "0.9.1"
        self.write_version(self.version)
        self.write_status(INFERENCE_ABI)
        (self.support / "version-exit.txt").write_text("0")
        (self.root / "bin").mkdir()
        self.scorer = self.root / "bin/llm-cc"
        self.scorer.write_text(
            "#!"
            + sys.executable
            + "\n"
            + SCORER_SOURCE.format(support=str(self.support))
        )
        self.scorer.chmod(0o755)
        backend_dir = self.root / "lib/llm-cc/backends/linux-x86_64"
        backend_dir.mkdir(parents=True)
        for backend in ("cuda", "rocm"):
            bundle = (backend + " bundle").encode()
            (backend_dir / f"{backend}.bundle").write_bytes(bundle)
            self.write_manifest(
                backend,
                sha256=hashlib.sha256(bundle).hexdigest(),
                size=len(bundle),
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

    def manifest_path(self, backend="rocm"):
        return self.root / f"lib/llm-cc/backends/linux-x86_64/{backend}.manifest.json"

    def write_manifest(self, backend="rocm", **overrides):
        path = self.manifest_path(backend)
        manifest = json.loads(path.read_text()) if path.exists() else {}
        manifest.update(
            {
                "name": backend,
                "git_sha": SOURCE_COMMIT,
                "version": self.version,
                "llama_cpp_commit": LLAMA_CPP_COMMIT,
                "configuration": "e" * 64,
                "ggml_backend_api_version": "1",
                **overrides,
            }
        )
        path.write_text(json.dumps(manifest))
        return manifest

    def write_version(self, version):
        (self.support / "version.txt").write_text("llm-cc " + version + "\n")

    def write_status(self, inference_abi, **overrides):
        status = {
            "scope": "user",
            "directory": str(self.support),
            "storage_version": 2,
            "inference_abi": inference_abi,
            "source_commit": SOURCE_COMMIT,
            "entries": 0,
            "bytes": 0,
            **overrides,
        }
        (self.support / "status.json").write_text(json.dumps(status))

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
            hashlib.sha256(self.scorer.read_bytes()).hexdigest(),
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

    def test_dogfood_preset_is_byte_identical(self):
        profile = self.rocm()
        self.assertEqual(profile["scoring"]["argv"], FROZEN_ARGV)
        self.assertEqual(profile["scoring"]["expected_configuration"], FROZEN_EXPECTED)
        self.assertEqual(
            sorted(profile["build"]),
            [
                "backend_manifest",
                "execution_host",
                "execution_image",
                "inference_abi",
                "installed_files",
                "model_bytes",
                "model_sha256",
                "model_url",
                "source_commit",
                "source_commit_verified",
            ],
        )
        self.assertEqual(profile["build"]["source_commit"], SOURCE_COMMIT)
        self.assertEqual(profile["build"]["inference_abi"], INFERENCE_ABI)
        self.assertNotIn("version", profile["build"])

    def test_argv_and_expected_configuration_come_from_one_contract(self):
        base = ScoringSettings(backend="rocm")
        variants = {
            "backend": "cuda",
            "gpu_layers": 20,
            "context": 65536,
            "batch_size": 64,
            "flash_attn": "off",
            "kv_cache_type": "q4_0",
            "kv_offload": "off",
            "entropy_reduction": "host",
            "hierarchy": "reference",
            "tau": 0.5,
            "alpha": 0.25,
            "score_mode": "mean",
            "hotspots": 3,
        }
        self.assertEqual(
            sorted(variants), sorted(f.name for f in dataclasses.fields(base))
        )
        for field, value in variants.items():
            start = {**base.__dict__, **LEGAL_COMPANIONS.get(field, {})}
            unchanged = ScoringSettings(**start)
            changed = ScoringSettings(**{**start, field: value})
            with self.subTest(field=field):
                self.assertNotEqual(unchanged.argv(), changed.argv())
                self.assertNotEqual(
                    unchanged.expected_configuration(INFERENCE_ABI),
                    changed.expected_configuration(INFERENCE_ABI),
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
        installation = inspect_installation(self.root, "rocm")
        model = ModelSpec("f" * 64, 123)
        variants.append(
            build_profile(
                installation,
                model,
                ScoringSettings(backend="rocm"),
                execution_image="none",
                execution_host=self.host,
            )
        )
        for changed in variants:
            self.assertNotEqual(digest(original), digest(changed))

    def test_scoring_setting_changes_change_the_fingerprint(self):
        installation = inspect_installation(self.root, "rocm")
        model = ModelSpec("f" * 64, 123, "https://example.invalid/model.gguf")

        def profile(settings=None, spec=None):
            return build_profile(
                installation,
                spec or model,
                settings or ScoringSettings(backend="rocm"),
                execution_image="none",
                execution_host=self.host,
            )

        base = digest(profile())
        seen = {base}
        for field, value in (
            ("gpu_layers", 20),
            ("context", 65536),
            ("batch_size", 64),
            ("flash_attn", "off"),
            ("kv_cache_type", "q4_0"),
            ("kv_offload", "off"),
            ("entropy_reduction", "host"),
            ("hierarchy", "reference"),
            ("tau", 0.5),
            ("alpha", 0.25),
            ("score_mode", "mean"),
            ("hotspots", 3),
        ):
            settings = ScoringSettings(
                **{
                    **ScoringSettings(backend="rocm").__dict__,
                    **LEGAL_COMPANIONS.get(field, {}),
                    field: value,
                }
            )
            with self.subTest(field=field):
                fingerprint = digest(profile(settings))
                self.assertNotIn(fingerprint, seen)
                seen.add(fingerprint)
        for spec in (
            ModelSpec("e" * 64, 123, "https://example.invalid/model.gguf"),
            ModelSpec("f" * 64, 124, "https://example.invalid/model.gguf"),
        ):
            with self.subTest(model=spec):
                self.assertNotIn(digest(profile(spec=spec)), seen)
        self.scorer.write_text(self.scorer.read_text() + "# changed\n")
        self.scorer.chmod(0o755)
        self.assertNotIn(
            digest(
                build_profile(
                    inspect_installation(self.root, "rocm"),
                    model,
                    ScoringSettings(backend="rocm"),
                    execution_image="none",
                    execution_host=self.host,
                )
            ),
            seen,
        )
        cuda = build_profile(
            inspect_installation(self.root, "cuda"),
            model,
            ScoringSettings(backend="cuda"),
            execution_image=self.image,
        )
        self.assertNotIn(digest(cuda), seen)

    def test_general_profile_for_unpinned_scorer(self):
        other_commit = "b" * 40
        other_llama = "c" * 40
        self.write_manifest(
            "rocm",
            git_sha=other_commit,
            llama_cpp_commit=other_llama,
            version="1.4.0",
        )
        self.write_version("1.4.0")
        abi = "llama.cpp-" + other_llama + "/entropy-v9"
        self.write_status(abi, source_commit=other_commit)
        installation = inspect_installation(self.root, "rocm")
        self.assertEqual(installation.source_commit, other_commit)
        self.assertEqual(installation.inference_abi, abi)
        self.assertEqual(installation.version, "1.4.0")
        profile = build_profile(
            installation,
            ModelSpec("f" * 64, 4096),
            ScoringSettings(backend="rocm", hotspots=5),
            execution_image="none",
            execution_host=self.host,
        )
        self.assertEqual(profile["build"]["source_commit"], other_commit)
        self.assertEqual(
            profile["scoring"]["expected_configuration"]["inference_abi"], abi
        )
        self.assertEqual(profile["scoring"]["expected_configuration"]["hotspots"], 5)
        self.assertNotIn("model_url", profile["build"])
        with self.assertRaisesRegex(ValueError, "must be built from"):
            self.rocm()

    def test_installation_mismatches_rejected(self):
        with self.subTest("version"):
            self.write_version("9.9.9")
            with self.assertRaisesRegex(ValueError, "manifest declares version"):
                self.rocm()
            self.write_version(self.version)
        with self.subTest("abi llama.cpp commit"):
            self.write_status("llama.cpp-" + "d" * 40 + "/entropy-v3")
            with self.assertRaisesRegex(ValueError, "built against llama.cpp"):
                inspect_installation(self.root, "rocm")
            self.write_status(INFERENCE_ABI)
        with self.subTest("garbage version"):
            (self.support / "version.txt").write_text("not a version\n")
            with self.assertRaisesRegex(ValueError, "manifest declares version"):
                self.rocm()
            self.write_version(self.version)
        with self.subTest("non-json cache status"):
            (self.support / "status.json").write_text("<html>")
            with self.assertRaisesRegex(ValueError, "did not produce JSON"):
                self.rocm()
        with self.subTest("cache status without abi"):
            self.write_status("nonsense")
            with self.assertRaisesRegex(ValueError, "valid inference_abi"):
                self.rocm()
            self.write_status(INFERENCE_ABI, storage_version="2")
            with self.assertRaisesRegex(ValueError, "storage_version"):
                self.rocm()
            self.write_status(INFERENCE_ABI)
        with self.subTest("non-zero exit"):
            (self.support / "version-exit.txt").write_text("3")
            with self.assertRaisesRegex(ValueError, "exit 3"):
                self.rocm()
            (self.support / "version-exit.txt").write_text("0")
        with self.subTest("incomplete manifest"):
            for field, value in (
                ("version", 7),
                ("llama_cpp_commit", "main"),
                ("configuration", "abc"),
                ("ggml_backend_api_version", None),
            ):
                self.write_manifest("rocm", **{field: value})
                with self.assertRaisesRegex(ValueError, "manifest is incomplete"):
                    self.rocm()
            self.write_manifest("rocm")
        with self.subTest("abi pin"):
            self.write_status("llama.cpp-" + LLAMA_CPP_COMMIT + "/entropy-v4")
            with self.assertRaisesRegex(ValueError, "inference ABI"):
                self.rocm()
            self.write_status(INFERENCE_ABI)
        with self.subTest("source commit"):
            # A mixed installation shares the version and ABI but not the
            # commit the executable resolves backends with.
            self.write_status(INFERENCE_ABI, source_commit="b" * 40)
            with self.assertRaisesRegex(ValueError, "built from commit"):
                self.rocm()
            for value in ("", "main", SOURCE_COMMIT.upper()):
                self.write_status(INFERENCE_ABI, source_commit=value)
                with self.assertRaisesRegex(ValueError, "valid source_commit"):
                    self.rocm()
            self.write_status(INFERENCE_ABI)
        with self.subTest("analysis version"):
            for value in (0, -1, "3", None):
                self.write_status(INFERENCE_ABI, analysis_version=value)
                with self.assertRaisesRegex(ValueError, "valid analysis_version"):
                    self.rocm()
            self.write_status(INFERENCE_ABI)

    def test_scorer_predating_the_probes_falls_back_to_pinned_values(self):
        # The pinned dogfood executable reports neither field; its commit is
        # then only the manifest's claim, and its analysis version is pinned.
        status = json.loads((self.support / "status.json").read_text())
        status.pop("source_commit")
        (self.support / "status.json").write_text(json.dumps(status))
        installation = inspect_installation(self.root, "rocm")
        self.assertEqual(installation.source_commit, SOURCE_COMMIT)
        self.assertFalse(installation.source_commit_verified)
        self.assertEqual(installation.analysis_version, PINNED_ANALYSIS_VERSION)
        profile = self.rocm()
        self.assertFalse(profile["build"]["source_commit_verified"])
        self.assertEqual(
            profile["scoring"]["expected_configuration"]["analysis_version"],
            PINNED_ANALYSIS_VERSION,
        )

    def test_reported_analysis_version_reaches_the_expected_configuration(self):
        self.write_status(INFERENCE_ABI, analysis_version=3)
        installation = inspect_installation(self.root, "rocm")
        self.assertEqual(installation.analysis_version, 3)
        self.assertTrue(installation.source_commit_verified)
        profile = self.rocm()
        self.assertEqual(
            profile["scoring"]["expected_configuration"]["analysis_version"], 3
        )

    def test_invalid_scorer_setting_combinations_rejected(self):
        # Each pair parses as individually valid but is refused by every
        # scorer run, so a profile carrying it could never be executed.
        with self.assertRaisesRegex(ValueError, "requires flash_attn on"):
            ScoringSettings(backend="rocm", flash_attn="off")
        with self.assertRaisesRegex(ValueError, "requires gpu_layers -1"):
            ScoringSettings(backend="rocm", gpu_layers=20)
        ScoringSettings(backend="rocm", flash_attn="off", kv_cache_type="f16")
        ScoringSettings(backend="rocm", gpu_layers=20, entropy_reduction="host")

    def test_gpu_layers_outside_int32_rejected(self):
        # The scorer parses --gpu-layers as int32 and refuses anything wider.
        ScoringSettings(backend="rocm", gpu_layers=2147483647,
                        entropy_reduction="host")
        with self.assertRaisesRegex(ValueError, "int32"):
            ScoringSettings(
                backend="rocm", gpu_layers=2147483648, entropy_reduction="host"
            )

    def test_model_changed_while_hashing_is_rejected(self):
        models = self.root / "unstable"
        models.mkdir()
        model = models / "model.gguf"
        model.write_bytes(b"original")
        real = profile_module.digest_file

        def swap(path):
            measured = real(path)
            path.write_bytes(b"replaced with other contents")
            return measured

        with unittest.mock.patch.object(profile_module, "digest_file", swap):
            with self.assertRaisesRegex(ValueError, "changed while being hashed"):
                ModelSpec.from_path(model)

    def test_split_model_identity_covers_every_shard(self):
        models = self.root / "models"
        models.mkdir()
        shards = []
        for index in (1, 2):
            shard = models / ("m-%05d-of-00002.gguf" % index)
            shard.write_bytes(b"shard-%d" % index)
            shards.append(shard)
        composite = hashlib.sha256(b"llm-cc-split-model-v1")
        for shard in shards:
            composite.update(b"\0")
            composite.update(
                hashlib.sha256(shard.read_bytes()).hexdigest().encode("ascii")
            )
        for named in shards:
            spec = ModelSpec.from_path(named)
            self.assertEqual(spec.sha256, composite.hexdigest())
            self.assertEqual(spec.bytes, sum(s.stat().st_size for s in shards))
        single = models / "plain.gguf"
        single.write_bytes(b"plain")
        self.assertEqual(
            ModelSpec.from_path(single).sha256,
            hashlib.sha256(b"plain").hexdigest(),
        )
        # An index outside its own count is not a split name to the scorer.
        odd = models / "m-00003-of-00002.gguf"
        odd.write_bytes(b"odd")
        self.assertEqual(
            ModelSpec.from_path(odd).sha256, hashlib.sha256(b"odd").hexdigest()
        )
        shards[1].unlink()
        with self.assertRaisesRegex(ValueError, "cannot resolve model shard"):
            ModelSpec.from_path(shards[0])

    def test_cache_status_is_not_scoped_to_a_repository(self):
        # A positional path is read as the repository to report on and is
        # rejected outside a Git worktree, which a temporary sandbox is.
        self.rocm()
        record = json.loads((self.support / "status-call.json").read_text())
        self.assertEqual(record["argv"], ["cache", "status", "--format", "json"])
        self.assertEqual(record["entropy"], str(Path(record["home"]) / "entropy"))

    def test_scorer_runs_offline_in_sanitized_environment(self):
        before = {
            path: path.stat().st_mtime_ns
            for path in sorted(self.root.rglob("*"))
            if path.is_file()
        }
        self.rocm()
        for name in ("version", "status"):
            record = json.loads((self.support / (name + "-call.json")).read_text())
            self.assertEqual(set(record["environment"]), SANITIZED_ENVIRONMENT)
            self.assertEqual(record["runtime"], str(self.root / "lib/llm-cc"))
            self.assertEqual(record["cwd"], record["home"])
            self.assertTrue(record["entropy"].startswith(record["home"]))
            self.assertFalse(record["entropy"].startswith(str(self.root)))
            self.assertFalse(Path(record["entropy"]).exists())
        after = {
            path: path.stat().st_mtime_ns
            for path in sorted(self.root.rglob("*"))
            if path.is_file()
        }
        self.assertEqual(before, after)

    def test_model_spec_from_local_file_and_mismatch(self):
        model = self.support / "model.gguf"
        model.write_bytes(b"gguf payload")
        expected = hashlib.sha256(b"gguf payload").hexdigest()
        spec = ModelSpec.from_path(model)
        self.assertEqual(spec.sha256, expected)
        self.assertEqual(spec.bytes, len(b"gguf payload"))
        self.assertIsNone(spec.url)
        agreed = ModelSpec.from_path(
            model, expected, len(b"gguf payload"), "https://example.invalid/m.gguf"
        )
        self.assertEqual(agreed.url, "https://example.invalid/m.gguf")
        for sha256, size in ((("a" * 64), None), (None, 5)):
            with self.subTest(sha256=sha256, size=size), self.assertRaises(ValueError):
                ModelSpec.from_path(model, sha256, size)
        for sha256, size, url in (
            ("A" * 64, 5, None),
            ("a" * 63, 5, None),
            ("a" * 64, 0, None),
            ("a" * 64, True, None),
            ("a" * 64, 5, "http://example.invalid/m.gguf"),
        ):
            with self.subTest(sha256=sha256), self.assertRaises(ValueError):
                ModelSpec(sha256, size, url)

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
        for kwargs in (
            {"backend": "metal"},
            {"gpu_layers": -2},
            {"gpu_layers": True},
            {"hotspots": -1},
            {"hotspots": True},
            {"tau": 0},
            {"tau": float("nan")},
            {"tau": "0.67"},
            {"alpha": -0.1},
            {"alpha": 1.5},
            {"alpha": float("inf")},
            {"flash_attn": "auto"},
            {"entropy_reduction": "auto"},
            {"kv_offload": "maybe"},
            {"hierarchy": "flat"},
            {"score_mode": "entropy"},
        ):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                ScoringSettings(backend=kwargs.pop("backend", "rocm"), **kwargs)

    def test_unpinned_backend_rejected(self):
        self.write_manifest("rocm", git_sha="main")
        with self.assertRaisesRegex(ValueError, SOURCE_COMMIT):
            self.rocm()

    @unittest.skipUnless(os.name == "posix", "symlink creation requires POSIX")
    def test_installed_symlink_rejected(self):
        (self.root / "bin/link").symlink_to("llm-cc")
        with self.assertRaisesRegex(ValueError, "symlinks"):
            self.rocm()

    def test_manifest_must_describe_installed_bundle(self):
        manifest = self.manifest_path("rocm")
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

    def test_missing_executable_rejected(self):
        self.scorer.unlink()
        with self.assertRaisesRegex(ValueError, "bin/llm-cc is missing"):
            self.rocm()

    def test_command_line_preserves_documented_invocation(self):
        host_file = self.support / "host.json"
        host_file.write_text(json.dumps(self.host))
        output = self.support / "profile.json"
        main(
            [
                "--installed-root",
                str(self.root),
                "--backend",
                "rocm",
                "--execution-host",
                str(host_file),
                "--output",
                str(output),
            ]
        )
        profile = json.loads(output.read_text())
        self.assertEqual(profile["scoring"]["argv"], FROZEN_ARGV)
        self.assertEqual(digest(profile), digest(self.rocm()))

    def test_command_line_generate_requires_a_model(self):
        output = self.support / "profile.json"
        arguments = [
            "generate",
            "--installed-root",
            str(self.root),
            "--backend",
            "cuda",
            "--execution-image",
            self.image,
            "--output",
            str(output),
        ]
        with self.assertRaisesRegex(ValueError, "--model"):
            main(arguments)
        model = self.support / "model.gguf"
        model.write_bytes(b"gguf payload")
        main(arguments + ["--model", str(model), "--hotspots", "4"])
        profile = json.loads(output.read_text())
        self.assertEqual(
            profile["build"]["model_sha256"],
            hashlib.sha256(b"gguf payload").hexdigest(),
        )
        self.assertEqual(profile["build"]["model_bytes"], len(b"gguf payload"))
        self.assertEqual(profile["scoring"]["expected_configuration"]["hotspots"], 4)
        argv = profile["scoring"]["argv"]
        self.assertEqual(argv[argv.index("--hotspots") + 1], "4")


if __name__ == "__main__":
    unittest.main()
