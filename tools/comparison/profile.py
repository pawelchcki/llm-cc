"""Materialize a scoring identity from an installed scorer tree.

The generator derives the scorer identity by executing the installed
executable offline (`bin/llm-cc --version` and `bin/llm-cc cache status
--format json`); it never trusts an operator-supplied commit or ABI.  The
dogfood preset keeps its pinned constants as cross-checks.
"""

import argparse
import dataclasses
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from .common import CONTAINER_ENVIRONMENT_POLICY

SOURCE_COMMIT = "4646123b274005c587dfeb614f17ddf5fd36aef6"
MODEL_SHA256 = "5a2e25280075d769abdb111de8211d9d3367f2ae0d0e6166a288ee6e8ed0345d"
MODEL_BYTES = 14066972416
MODEL_URL = (
    "https://huggingface.co/bartowski/DeepSeek-Coder-V2-Lite-Base-GGUF/resolve/"
    "babc004c32d74af2553661d912673f1d22dcb3f1/DeepSeek-Coder-V2-Lite-Base-Q6_K.gguf"
)
INFERENCE_ABI = "llama.cpp-c589f0ed10c643678c4707dd160c21ac7633ebc0/entropy-v3"

BACKENDS = ("cuda", "rocm")
DEFAULT_CONTEXT = {"rocm": 32768, "cuda": 131072}
FLASH_ATTENTION = ("on", "off")
KV_CACHE_TYPES = ("f16", "q8_0", "q4_0")
KV_OFFLOAD = ("on", "off")
ENTROPY_REDUCTIONS = ("device", "host")
HIERARCHIES = ("structural", "reference")
SCORE_MODES = ("lmcc", "density", "mean")
INFERENCE_ABI_PATTERN = r"llama\.cpp-[0-9a-f]{40}/entropy-v\d+"


def digest_file(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _text(value):
    return str(value)


@dataclasses.dataclass(frozen=True)
class _Flag:
    """One scorer flag and the configuration keys it must be echoed in."""

    flag: str
    field: str | None = None
    constant: str | None = None
    render: object = _text
    expected_keys: tuple = ()
    derived: object = None

    def value(self, settings):
        return self.constant if self.field is None else getattr(settings, self.field)


# Single source of truth: `argv()` and `expected_configuration()` are both
# generated from this table so the launched flags and the keys the worker
# validates cannot drift apart.
CONTRACT = (
    _Flag(
        "--backend",
        "backend",
        derived=lambda s: {"backend": f"{s.backend}/gpu-layers={s.gpu_layers}"},
    ),
    _Flag("--gpu-layers", "gpu_layers", expected_keys=("gpu_layers",)),
    _Flag("--context", "context", expected_keys=("context",)),
    _Flag("--batch-size", "batch_size", expected_keys=("batch_size",)),
    _Flag(
        "--flash-attn",
        "flash_attn",
        expected_keys=("flash_attn", "effective_flash_attn"),
    ),
    _Flag(
        "--kv-cache-type",
        "kv_cache_type",
        expected_keys=("kv_cache_type", "effective_kv_cache_type"),
    ),
    _Flag(
        "--kv-offload",
        "kv_offload",
        expected_keys=("kv_offload", "effective_kv_offload"),
    ),
    _Flag(
        "--entropy-reduction",
        "entropy_reduction",
        expected_keys=("entropy_reduction", "effective_entropy_reducer"),
    ),
    _Flag("--hierarchy", "hierarchy", expected_keys=("hierarchy_mode",)),
    _Flag("--tau", "tau", render=repr, expected_keys=("tau",)),
    _Flag("--alpha", "alpha", render=repr, expected_keys=("alpha",)),
    _Flag("--score", "score_mode", expected_keys=("score_mode",)),
    _Flag("--progress", constant="always", expected_keys=("progress",)),
    _Flag("--hotspots", "hotspots", expected_keys=("hotspots",)),
)

# Configuration keys the scorer always reports for the orchestrated flags the
# worker itself supplies, plus derived constants.
CONSTANT_CONFIGURATION = {
    "analysis_version": 2,
    "include_headers": True,
    "no_ignore": True,
    "no_download": True,
    "backend_diagnostics": False,
    "tau_rule": "absolute",
    "tau_percentile": None,
}


def _positive_int(name, value, limit=4294967295):
    if type(value) is not int or not 0 < value <= limit:
        raise ValueError(f"{name} must be a positive 32-bit integer")


def _finite(name, value):
    if type(value) not in (int, float) or not math.isfinite(value):
        raise ValueError(f"{name} must be a finite number")


def _choice(name, value, choices):
    if value not in choices:
        raise ValueError(f"{name} must be one of {', '.join(choices)}")


@dataclasses.dataclass(frozen=True)
class ScoringSettings:
    """Deterministic scorer settings shared by argv and expected configuration."""

    backend: str = "cuda"
    gpu_layers: int = -1
    context: int | None = None
    batch_size: int = 256
    flash_attn: str = "on"
    kv_cache_type: str = "q8_0"
    kv_offload: str = "on"
    entropy_reduction: str = "device"
    hierarchy: str = "structural"
    tau: float = 0.67
    alpha: float = 0.8
    score_mode: str = "lmcc"
    hotspots: int = 0

    def __post_init__(self):
        _choice("backend", self.backend, BACKENDS)
        if self.context is None:
            object.__setattr__(self, "context", DEFAULT_CONTEXT[self.backend])
        self.validate()

    def validate(self):
        _choice("backend", self.backend, BACKENDS)
        _positive_int("context", self.context)
        _positive_int("batch_size", self.batch_size)
        if type(self.gpu_layers) is not int or self.gpu_layers < -1:
            raise ValueError("gpu_layers must be an integer >= -1")
        if type(self.hotspots) is not int or self.hotspots < 0:
            raise ValueError("hotspots must be a non-negative integer")
        # `auto` resolves against the runtime, so the scorer's effective_*
        # values would stop being predictable from the profile alone.
        _choice("flash_attn", self.flash_attn, FLASH_ATTENTION)
        _choice("kv_cache_type", self.kv_cache_type, KV_CACHE_TYPES)
        _choice("kv_offload", self.kv_offload, KV_OFFLOAD)
        _choice("entropy_reduction", self.entropy_reduction, ENTROPY_REDUCTIONS)
        _choice("hierarchy", self.hierarchy, HIERARCHIES)
        _choice("score_mode", self.score_mode, SCORE_MODES)
        _finite("tau", self.tau)
        if self.tau <= 0:
            raise ValueError("tau must be greater than zero")
        _finite("alpha", self.alpha)
        if not 0 <= self.alpha <= 1:
            raise ValueError("alpha must be between zero and one")

    def argv(self):
        argv = []
        for row in CONTRACT:
            argv.extend([row.flag, row.render(row.value(self))])
        return argv

    def expected_configuration(self, inference_abi):
        expected = dict(CONSTANT_CONFIGURATION)
        for row in CONTRACT:
            for key in row.expected_keys:
                expected[key] = row.value(self)
            if row.derived is not None:
                expected.update(row.derived(self))
        expected["inference_abi"] = inference_abi
        return expected


@dataclasses.dataclass(frozen=True)
class ModelSpec:
    """Digest-pinned identity of the GGUF model the scorer must load."""

    sha256: str
    bytes: int
    url: str | None = None

    def __post_init__(self):
        if not isinstance(self.sha256, str) or not re.fullmatch(
            r"[0-9a-f]{64}", self.sha256
        ):
            raise ValueError("model_sha256 must be a lowercase SHA-256 digest")
        if type(self.bytes) is not int or self.bytes <= 0:
            raise ValueError("model_bytes must be a positive integer")
        if self.url is not None and (
            not isinstance(self.url, str) or not self.url.startswith("https://")
        ):
            raise ValueError("model_url must be an https URL")

    @classmethod
    def from_path(cls, path, sha256=None, bytes=None, url=None):
        model = Path(path)
        measured = digest_file(model)
        size = model.stat().st_size
        if (sha256 is not None and sha256 != measured) or (
            bytes is not None and bytes != size
        ):
            raise ValueError("model file does not match the declared digest or size")
        return cls(measured, size, url)


@dataclasses.dataclass(frozen=True)
class ScorerInstallation:
    """Identity of an installed scorer tree, derived by executing it."""

    root: str
    backend: str
    version: str
    source_commit: str
    inference_abi: str
    manifest: dict
    installed_files: dict

    def __getitem__(self, key):
        return getattr(self, key)


def _sanitized_environment(sandbox, runtime_dir):
    return {
        "PATH": "/usr/bin:/bin",
        "LANG": "C.UTF-8",
        "HOME": str(sandbox),
        "LLM_CC_ENTROPY_CACHE_DIR": str(sandbox / "entropy"),
        "LLM_CC_CACHE_DIR": str(sandbox / "models"),
        "LLM_CC_RUNTIME_DIR": str(runtime_dir),
    }


def _run_scorer(executable, arguments, sandbox, runtime_dir, timeout):
    try:
        completed = subprocess.run(
            [str(executable), *arguments],
            env=_sanitized_environment(sandbox, runtime_dir),
            cwd=str(sandbox),
            stdin=subprocess.DEVNULL,
            capture_output=True,
            timeout=timeout,
            start_new_session=True,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise ValueError(
            "installed scorer timed out running " + " ".join(arguments)
        ) from error
    except OSError as error:
        raise ValueError(
            "installed scorer could not be executed: " + str(error)
        ) from error
    if completed.returncode != 0:
        raise ValueError(
            "installed scorer failed running %s (exit %d)"
            % (" ".join(arguments), completed.returncode)
        )
    try:
        return completed.stdout.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValueError("installed scorer output is not UTF-8") from error


def _validate_manifest(manifest):
    if (
        not isinstance(manifest, dict)
        or not isinstance(manifest.get("git_sha"), str)
        or not re.fullmatch(r"[0-9a-f]{40}", manifest["git_sha"])
        or not isinstance(manifest.get("version"), str)
        or not manifest["version"]
        or not isinstance(manifest.get("llama_cpp_commit"), str)
        or not re.fullmatch(r"[0-9a-f]{40}", manifest["llama_cpp_commit"])
        or not isinstance(manifest.get("configuration"), str)
        or not re.fullmatch(r"[0-9a-f]{64}", manifest["configuration"])
        or not isinstance(manifest.get("ggml_backend_api_version"), str)
        or not manifest["ggml_backend_api_version"]
    ):
        raise ValueError("backend manifest is incomplete")


def inspect_installation(
    installed_root,
    backend,
    *,
    timeout=60,
    expected_source_commit=None,
    expected_inference_abi=None,
):
    """Hash an installed tree and execute it offline to derive its identity."""
    _choice("backend", backend, BACKENDS)
    root = Path(installed_root).resolve(strict=True)
    manifests = sorted(root.glob(f"lib/llm-cc/backends/**/{backend}.manifest.json"))
    if len(manifests) != 1:
        raise ValueError(
            f"install exactly one pinned {backend.upper()} backend before creating the profile"
        )
    manifest = json.loads(manifests[0].read_text())
    if expected_source_commit is not None and (
        not isinstance(manifest, dict)
        or manifest.get("git_sha") != expected_source_commit
    ):
        raise ValueError(
            f"{backend.upper()} backend must be built from {expected_source_commit}"
        )
    _validate_manifest(manifest)
    files = {}
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"installed identity cannot contain symlinks: {path}")
        if path.is_file():
            files[path.relative_to(root).as_posix()] = digest_file(path)
    if "bin/llm-cc" not in files:
        raise ValueError("installed bin/llm-cc is missing")
    bundle = manifests[0].with_name(f"{backend}.bundle")
    if (
        not bundle.is_file()
        or manifest.get("sha256") != files.get(bundle.relative_to(root).as_posix())
        or type(manifest.get("size")) is not int
        or manifest["size"] != bundle.stat().st_size
    ):
        raise ValueError(
            "backend manifest checksum or size does not match installed bundle"
        )
    executable = root / "bin/llm-cc"
    with tempfile.TemporaryDirectory() as sandbox_name:
        sandbox = Path(sandbox_name)
        (sandbox / "entropy").mkdir()
        (sandbox / "models").mkdir()
        runtime_dir = root / "lib/llm-cc"
        version = _run_scorer(
            executable, ["--version"], sandbox, runtime_dir, timeout
        ).strip()
        status_text = _run_scorer(
            executable,
            ["cache", "status", str(sandbox / "entropy"), "--format", "json"],
            sandbox,
            runtime_dir,
            timeout,
        )
    if version != "llm-cc " + manifest["version"]:
        raise ValueError(
            "installed executable reports %r but the backend manifest declares version %r"
            % (version, manifest["version"])
        )
    try:
        status = json.loads(status_text)
    except json.JSONDecodeError as error:
        raise ValueError("cache status did not produce JSON") from error
    if not isinstance(status, dict) or type(status.get("storage_version")) is not int:
        raise ValueError("cache status JSON is missing storage_version")
    inference_abi = status.get("inference_abi")
    if not isinstance(inference_abi, str) or not re.fullmatch(
        INFERENCE_ABI_PATTERN, inference_abi
    ):
        raise ValueError("cache status JSON is missing a valid inference_abi")
    if not inference_abi.startswith("llama.cpp-" + manifest["llama_cpp_commit"] + "/"):
        raise ValueError(
            "installed executable reports inference ABI %r but the backend was built "
            "against llama.cpp %s" % (inference_abi, manifest["llama_cpp_commit"])
        )
    if expected_inference_abi is not None and inference_abi != expected_inference_abi:
        raise ValueError(
            f"{backend.upper()} backend must report inference ABI {expected_inference_abi}"
        )
    return ScorerInstallation(
        root=str(root),
        backend=backend,
        version=manifest["version"],
        source_commit=manifest["git_sha"],
        inference_abi=inference_abi,
        manifest=manifest,
        installed_files=files,
    )


def _validate_execution_host(host):
    if not isinstance(host, dict):
        raise ValueError("bare execution requires an execution_host object")
    if (
        host.get("gpu_vendor") != "amd"
        or not isinstance(host.get("gpu_arch"), str)
        or not re.fullmatch(r"gfx[0-9a-f]+", host["gpu_arch"])
        or not isinstance(host.get("gpu_pci_address"), str)
        or not re.fullmatch(
            r"[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]", host["gpu_pci_address"]
        )
        or not isinstance(host.get("resource_id"), str)
        or not re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,63}", host["resource_id"])
        or type(host.get("gpu_vram_bytes_min")) is not int
        or host["gpu_vram_bytes_min"] <= 0
    ):
        raise ValueError(
            "execution_host requires explicit AMD GPU and resource identity"
        )
    files = host.get("runtime_files")
    if not isinstance(files, dict) or not files:
        raise ValueError("execution_host requires checksummed runtime_files")
    for path, checksum in files.items():
        if (
            not isinstance(path, str)
            or not Path(path).is_absolute()
            or ".." in Path(path).parts
            or not isinstance(checksum, str)
            or not re.fullmatch(r"[0-9a-f]{64}", checksum)
        ):
            raise ValueError("runtime_files must map absolute paths to SHA-256 digests")


def validate_execution(execution_image, execution_host, backend, max_file_bytes):
    """Reject mutable images and incomplete bare-host identities."""
    if execution_image == "none":
        if backend != "rocm":
            raise ValueError("bare dogfooding requires the ROCm backend")
        _validate_execution_host(execution_host)
    elif not isinstance(execution_image, str) or not re.fullmatch(
        r"[^\s]+@sha256:[0-9a-f]{64}", execution_image
    ):
        raise ValueError("execution image must be pinned by its SHA-256 digest")
    elif execution_host is not None:
        raise ValueError("execution_host requires execution_image='none'")
    if type(max_file_bytes) is not int or max_file_bytes <= 0:
        raise ValueError("max_file_bytes must be a positive integer")


def build_profile(
    installation,
    model,
    settings,
    *,
    execution_image,
    execution_host=None,
    max_file_bytes=65536,
    expected_source_commit=None,
    expected_inference_abi=None,
):
    """Assemble the profile document from a derived installation identity."""
    settings.validate()
    if installation.backend != settings.backend:
        raise ValueError("settings backend differs from the inspected installation")
    validate_execution(
        execution_image, execution_host, settings.backend, max_file_bytes
    )
    if (
        expected_source_commit is not None
        and installation.source_commit != expected_source_commit
    ):
        raise ValueError(
            f"{settings.backend.upper()} backend must be built from {expected_source_commit}"
        )
    if (
        expected_inference_abi is not None
        and installation.inference_abi != expected_inference_abi
    ):
        raise ValueError(
            f"{settings.backend.upper()} backend must report inference ABI "
            + expected_inference_abi
        )
    build = {
        "source_commit": installation.source_commit,
        "inference_abi": installation.inference_abi,
        "installed_files": installation.installed_files,
        "backend_manifest": installation.manifest,
        "execution_image": execution_image,
        "model_sha256": model.sha256,
        "model_bytes": model.bytes,
    }
    if model.url is not None:
        build["model_url"] = model.url
    profile = {
        "max_file_bytes": max_file_bytes,
        "scoring": {
            "argv": settings.argv(),
            "expected_configuration": settings.expected_configuration(
                installation.inference_abi
            ),
        },
        "build": build,
    }
    if execution_host is not None:
        # Copy through JSON so caller mutations cannot change a prepared contract.
        profile["build"]["execution_host"] = json.loads(json.dumps(execution_host))
    else:
        profile["build"]["container_environment_policy"] = CONTAINER_ENVIRONMENT_POLICY
    return profile


def dogfood_profile(
    installed_root,
    execution_image,
    max_file_bytes=65536,
    *,
    backend="cuda",
    context=None,
    batch_size=256,
    kv_cache_type="q8_0",
    execution_host=None,
):
    """Preset that pins the shared dogfood identity to a known scorer build."""
    settings = ScoringSettings(
        backend=backend,
        context=context,
        batch_size=batch_size,
        kv_cache_type=kv_cache_type,
    )
    validate_execution(execution_image, execution_host, backend, max_file_bytes)
    installation = inspect_installation(
        installed_root,
        backend,
        expected_source_commit=SOURCE_COMMIT,
        expected_inference_abi=INFERENCE_ABI,
    )
    return build_profile(
        installation,
        ModelSpec(MODEL_SHA256, MODEL_BYTES, MODEL_URL),
        settings,
        execution_image=execution_image,
        execution_host=execution_host,
        max_file_bytes=max_file_bytes,
        expected_source_commit=SOURCE_COMMIT,
        expected_inference_abi=INFERENCE_ABI,
    )


def _add_execution_arguments(parser):
    parser.add_argument("--installed-root", required=True)
    parser.add_argument("--execution-image", default="none")
    parser.add_argument(
        "--execution-host",
        help="JSON AMD GPU identity and runtime checksums for bare execution",
    )
    parser.add_argument("--max-file-bytes", type=int, default=65536)
    parser.add_argument("--output", required=True)


def parser():
    result = argparse.ArgumentParser(
        prog="python -m tools.comparison.profile", description=__doc__
    )
    commands = result.add_subparsers(dest="command", required=True)
    preset = commands.add_parser("dogfood", help="pinned dogfood identity")
    _add_execution_arguments(preset)
    preset.add_argument("--backend", choices=BACKENDS, default="cuda")
    preset.add_argument("--context", type=int)
    preset.add_argument("--batch-size", type=int, default=256)
    preset.add_argument("--kv-cache-type", choices=KV_CACHE_TYPES, default="q8_0")
    general = commands.add_parser(
        "generate", help="identity derived from any installed scorer"
    )
    _add_execution_arguments(general)
    general.add_argument("--backend", choices=BACKENDS, required=True)
    general.add_argument("--model", help="local GGUF file to hash")
    general.add_argument("--model-sha256")
    general.add_argument("--model-bytes", type=int)
    general.add_argument("--model-url")
    general.add_argument("--expected-source-commit")
    general.add_argument("--expected-inference-abi")
    general.add_argument("--gpu-layers", type=int, default=-1)
    general.add_argument("--context", type=int)
    general.add_argument("--batch-size", type=int, default=256)
    general.add_argument("--flash-attn", choices=FLASH_ATTENTION, default="on")
    general.add_argument("--kv-cache-type", choices=KV_CACHE_TYPES, default="q8_0")
    general.add_argument("--kv-offload", choices=KV_OFFLOAD, default="on")
    general.add_argument(
        "--entropy-reduction", choices=ENTROPY_REDUCTIONS, default="device"
    )
    general.add_argument("--hierarchy", choices=HIERARCHIES, default="structural")
    general.add_argument("--tau", type=float, default=0.67)
    general.add_argument("--alpha", type=float, default=0.8)
    general.add_argument("--score-mode", choices=SCORE_MODES, default="lmcc")
    general.add_argument("--hotspots", type=int, default=0)
    return result


def _model_spec(args):
    if args.model:
        return ModelSpec.from_path(
            args.model, args.model_sha256, args.model_bytes, args.model_url
        )
    if not args.model_sha256 or args.model_bytes is None:
        raise ValueError(
            "generate requires --model PATH or --model-sha256 with --model-bytes"
        )
    return ModelSpec(args.model_sha256, args.model_bytes, args.model_url)


def _host(args):
    if not args.execution_host:
        return None
    return json.loads(Path(args.execution_host).read_text())


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] not in {"dogfood", "generate", "-h", "--help"}:
        # Backward compatibility with the documented flag-only invocation.
        argv.insert(0, "dogfood")
    args = parser().parse_args(argv)
    if args.command == "dogfood":
        profile = dogfood_profile(
            args.installed_root,
            args.execution_image,
            args.max_file_bytes,
            backend=args.backend,
            context=args.context,
            batch_size=args.batch_size,
            kv_cache_type=args.kv_cache_type,
            execution_host=_host(args),
        )
    else:
        settings = ScoringSettings(
            backend=args.backend,
            gpu_layers=args.gpu_layers,
            context=args.context,
            batch_size=args.batch_size,
            flash_attn=args.flash_attn,
            kv_cache_type=args.kv_cache_type,
            kv_offload=args.kv_offload,
            entropy_reduction=args.entropy_reduction,
            hierarchy=args.hierarchy,
            tau=args.tau,
            alpha=args.alpha,
            score_mode=args.score_mode,
            hotspots=args.hotspots,
        )
        model = _model_spec(args)
        validate_execution(
            args.execution_image, _host(args), args.backend, args.max_file_bytes
        )
        installation = inspect_installation(
            args.installed_root,
            args.backend,
            expected_source_commit=args.expected_source_commit,
            expected_inference_abi=args.expected_inference_abi,
        )
        profile = build_profile(
            installation,
            model,
            settings,
            execution_image=args.execution_image,
            execution_host=_host(args),
            max_file_bytes=args.max_file_bytes,
        )
    Path(args.output).write_text(json.dumps(profile, indent=2, allow_nan=False) + "\n")
    return 0


if __name__ == "__main__":
    main()
