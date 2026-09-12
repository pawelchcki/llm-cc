"""Materialize the pinned dogfood contract from an installed scorer tree."""

import argparse
import hashlib
import json
from pathlib import Path
import re

SOURCE_COMMIT = "4646123b274005c587dfeb614f17ddf5fd36aef6"
MODEL_SHA256 = "5a2e25280075d769abdb111de8211d9d3367f2ae0d0e6166a288ee6e8ed0345d"
MODEL_BYTES = 14066972416
MODEL_URL = (
    "https://huggingface.co/bartowski/DeepSeek-Coder-V2-Lite-Base-GGUF/resolve/"
    "babc004c32d74af2553661d912673f1d22dcb3f1/DeepSeek-Coder-V2-Lite-Base-Q6_K.gguf"
)
INFERENCE_ABI = "llama.cpp-c589f0ed10c643678c4707dd160c21ac7633ebc0/entropy-v3"


def digest_file(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


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
    root = Path(installed_root).resolve(strict=True)
    if backend not in {"cuda", "rocm"}:
        raise ValueError("backend must be cuda or rocm")
    if execution_image == "none":
        if backend != "rocm":
            raise ValueError("bare dogfooding requires the ROCm backend")
        _validate_execution_host(execution_host)
    elif not re.fullmatch(r"[^\s]+@sha256:[0-9a-f]{64}", execution_image):
        raise ValueError("execution image must be pinned by its SHA-256 digest")
    elif execution_host is not None:
        raise ValueError("execution_host requires execution_image='none'")
    if type(max_file_bytes) is not int or max_file_bytes <= 0:
        raise ValueError("max_file_bytes must be a positive integer")
    if context is None:
        context = 32768 if backend == "rocm" else 131072
    for name, value in (("context", context), ("batch_size", batch_size)):
        if type(value) is not int or not 0 < value <= 4294967295:
            raise ValueError(f"{name} must be a positive 32-bit integer")
    if kv_cache_type not in {"f16", "q8_0", "q4_0"}:
        raise ValueError("kv_cache_type must be f16, q8_0, or q4_0")
    manifests = sorted(root.glob(f"lib/llm-cc/backends/**/{backend}.manifest.json"))
    if len(manifests) != 1:
        raise ValueError(
            f"install exactly one pinned {backend.upper()} backend before creating the profile"
        )
    manifest = json.loads(manifests[0].read_text())
    if manifest.get("git_sha") != SOURCE_COMMIT:
        raise ValueError(
            f"{backend.upper()} backend must be built from {SOURCE_COMMIT}"
        )
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
    expected = {
        "analysis_version": 2,
        "hierarchy_mode": "structural",
        "include_headers": True,
        "no_ignore": True,
        "no_download": True,
        "progress": "always",
        "context": context,
        "batch_size": batch_size,
        "entropy_reduction": "device",
        "effective_entropy_reducer": "device",
        "flash_attn": "on",
        "effective_flash_attn": "on",
        "kv_cache_type": kv_cache_type,
        "effective_kv_cache_type": kv_cache_type,
        "kv_offload": "on",
        "effective_kv_offload": "on",
        "backend_diagnostics": False,
        "score_mode": "lmcc",
        "tau_rule": "absolute",
        "tau": 0.67,
        "tau_percentile": None,
        "alpha": 0.8,
        "hotspots": 0,
        "backend": f"{backend}/gpu-layers=-1",
        "gpu_layers": -1,
        "inference_abi": INFERENCE_ABI,
    }
    argv = [
        "--backend",
        backend,
        "--gpu-layers",
        "-1",
        "--context",
        str(context),
        "--batch-size",
        str(batch_size),
        "--flash-attn",
        "on",
        "--kv-cache-type",
        kv_cache_type,
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
    profile = {
        "max_file_bytes": max_file_bytes,
        "scoring": {"argv": argv, "expected_configuration": expected},
        "build": {
            "source_commit": SOURCE_COMMIT,
            "inference_abi": INFERENCE_ABI,
            "installed_files": files,
            "backend_manifest": manifest,
            "execution_image": execution_image,
            "model_sha256": MODEL_SHA256,
            "model_bytes": MODEL_BYTES,
            "model_url": MODEL_URL,
        },
    }
    if execution_host is not None:
        # Copy through JSON so caller mutations cannot change a prepared contract.
        profile["build"]["execution_host"] = json.loads(json.dumps(execution_host))
    return profile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--installed-root", required=True)
    parser.add_argument("--execution-image", default="none")
    parser.add_argument(
        "--execution-host",
        help="JSON AMD GPU identity and runtime checksums for bare execution",
    )
    parser.add_argument("--backend", choices=("cuda", "rocm"), default="cuda")
    parser.add_argument("--context", type=int)
    parser.add_argument("--batch-size", type=int, default=256)
    parser.add_argument(
        "--kv-cache-type", choices=("f16", "q8_0", "q4_0"), default="q8_0"
    )
    parser.add_argument("--max-file-bytes", type=int, default=65536)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    profile = dogfood_profile(
        args.installed_root,
        args.execution_image,
        args.max_file_bytes,
        backend=args.backend,
        context=args.context,
        batch_size=args.batch_size,
        kv_cache_type=args.kv_cache_type,
        execution_host=json.loads(Path(args.execution_host).read_text())
        if args.execution_host
        else None,
    )
    Path(args.output).write_text(json.dumps(profile, indent=2, allow_nan=False) + "\n")


if __name__ == "__main__":
    main()
