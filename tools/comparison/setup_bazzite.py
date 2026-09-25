"""Validate existing host assets and publish a reproducible dogfood configuration.

This command does not build the scorer, download models, or provision a store.
Run it after installing the pinned scorer and describing the host's GPU in an
execution-host JSON file.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shlex
import stat
import subprocess
import tempfile
import zipfile

from .buildbuddy import bundle_command, validate_report_links
from .common import canonical_bytes

# The dogfood ROCm scoring contract; every setting is explicit, one argument
# per line as `llm-cc compare` reads response files.
DOGFOOD_SCORING = (
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
)
PCI_ADDRESS = re.compile(r"[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]")
RESOURCE_ID = re.compile(r"[a-z0-9][a-z0-9_-]{0,63}")


def digest_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def package_bundle(source_root):
    """Capture only the Python implementation, never untracked host credentials."""
    root = Path(source_root).resolve(strict=True)
    package = root / "tools" / "comparison"
    paths = [root / "tools" / "__init__.py"]
    paths.extend(
        sorted(p for p in package.glob("*.py") if not p.name.startswith("test_"))
    )
    required = {
        "__init__.py",
        "__main__.py",
        "buildbuddy.py",
        "store.py",
        "submit_bazzite.py",
    }
    if not required.issubset({p.name for p in paths[1:]}):
        raise ValueError("source root does not contain the complete comparison package")
    result = io.BytesIO()
    with zipfile.ZipFile(result, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in paths:
            if (
                path.is_symlink()
                or not path.is_file()
                or root not in path.resolve().parents
            ):
                raise ValueError(
                    f"bundle source must be a regular file inside source root: {path}"
                )
            member = zipfile.ZipInfo(
                path.relative_to(root).as_posix(), (1980, 1, 1, 0, 0, 0)
            )
            member.compress_type = zipfile.ZIP_DEFLATED
            member.external_attr = 0o100644 << 16
            archive.writestr(member, path.read_bytes())
    payload = result.getvalue()
    return payload, hashlib.sha256(payload).hexdigest()


def atomic_publish(path, payload, mode=0o644, directory_mode=0o755):
    """Publish complete bytes, preserving any concurrent reader's prior file."""
    path = Path(path)
    # The non-root executor users must traverse and read every published
    # directory. mkdir applies the ambient umask, so a hardened root umask such
    # as 0077 would otherwise leave 0700 here and break every workflow.
    created = [
        parent
        for parent in (path.parent, *path.parent.parents)
        if not parent.exists()
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    for parent in created:
        os.chmod(parent, directory_mode)
    descriptor, temporary = tempfile.mkstemp(prefix=".publish-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fchmod(stream.fileno(), mode)
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        Path(temporary).unlink(missing_ok=True)


def launcher_script(bundle, checksum, generation_config):
    """Emit a one-argument entry point pinning the verified package and settings."""
    for path in (bundle, generation_config):
        if not Path(path).is_absolute():
            raise ValueError("launcher paths must be absolute")
    command = bundle_command(
        [
            "python3",
            "-m",
            "tools.comparison.submit_bazzite",
            "run-coordinator",
            "--config",
            str(generation_config),
        ],
        str(bundle),
        checksum,
    )
    return (
        "#!/usr/bin/env bash\n"
        "# Published by tools.comparison.setup_bazzite. Do not edit in place.\n"
        "set -euo pipefail\n"
        "exec " + shlex.join(command) + ' "$@"\n'
    ).encode("utf-8")


def verify_execution_host(host):
    """Check the bare-host contract llm-cc verifies before every worker."""
    if (
        not isinstance(host, dict)
        or host.get("gpu_vendor") != "amd"
        or host.get("gpu_arch") != "gfx1100"
        or not isinstance(host.get("gpu_pci_address"), str)
        or not PCI_ADDRESS.fullmatch(host["gpu_pci_address"])
        or not isinstance(host.get("resource_id"), str)
        or not RESOURCE_ID.fullmatch(host["resource_id"])
        or type(host.get("gpu_vram_bytes_min")) is not int
        or host["gpu_vram_bytes_min"] <= 0
    ):
        raise ValueError(
            "Bazzite setup requires an AMD gfx1100 execution host with its PCI "
            "address, minimum VRAM and resource_id"
        )
    files = host.get("runtime_files")
    if not isinstance(files, dict) or not files:
        raise ValueError("bare-host runtime checksums are required")
    for path, checksum in files.items():
        runtime = Path(path)
        if (
            not runtime.is_absolute()
            or not runtime.is_file()
            or digest_file(runtime) != checksum
        ):
            raise ValueError(f"runtime file does not match execution host: {path}")
    return host


def verify_assets(installed_root, model):
    root = Path(installed_root).resolve(strict=True)
    llm_cc = root / "bin/llm-cc"
    if llm_cc.is_symlink() or not llm_cc.is_file() or not os.access(llm_cc, os.X_OK):
        raise ValueError("installed root must contain an executable bin/llm-cc")
    model = Path(model).resolve(strict=True)
    if not model.is_file():
        raise ValueError("model must be an existing GGUF file")
    return llm_cc, model


def scoring_identity(llm_cc, model, scoring):
    """{scorer, model, scoring, fingerprint} as the pinned llm-cc derives it."""
    with tempfile.TemporaryDirectory(prefix="llm-cc-setup-") as temporary:
        arguments = Path(temporary) / "scoring.args"
        arguments.write_text("".join(line + "\n" for line in scoring))
        completed = subprocess.run(
            [
                str(llm_cc),
                "compare",
                "identity",
                "--model",
                str(model),
                "@" + str(arguments),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    if completed.returncode != 0:
        raise ValueError(
            "llm-cc compare identity failed: "
            + completed.stderr.decode("utf-8", "replace").strip()
        )
    return json.loads(completed.stdout)


def read_scoring_args(path):
    if path is None:
        return list(DOGFOOD_SCORING)
    lines = Path(path).read_text(encoding="utf-8").splitlines()
    return [
        line.strip()
        for line in lines
        if line.strip() and not line.strip().startswith("#")
    ]


def provision_lock(assets, resource_id, group=None):
    """Only root may establish the host-wide lock used by both executors."""
    import grp

    if os.geteuid() != 0:
        raise ValueError("run host setup as root to establish the shared GPU lock")
    if not re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,63}", resource_id):
        raise ValueError("invalid GPU resource_id")
    directory = assets / "locks"
    directory.mkdir(mode=0o755, parents=True, exist_ok=True)
    for parent in [directory, *directory.parents]:
        info = parent.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
            raise ValueError(
                "GPU lock ancestors must be root-owned directories without group/world write"
            )
    descriptor = os.open(
        directory / (resource_id + ".lock"),
        os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW,
        0o660,
    )
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != 0:
            raise ValueError("GPU lock must be a root-owned regular file")
        if group:
            os.fchown(descriptor, 0, grp.getgrnam(group).gr_gid)
        os.fchmod(descriptor, 0o660)
    finally:
        os.close(descriptor)


def configure(args):
    host = verify_execution_host(
        json.loads(Path(args.execution_host).read_text(encoding="utf-8"))
    )
    llm_cc, model = verify_assets(args.installed_root, args.model)
    scoring = read_scoring_args(args.scoring_args)
    if any(line.startswith(("@", "--model")) for line in scoring):
        raise ValueError("scoring arguments cannot name models or response files")
    identity = scoring_identity(llm_cc, model, scoring)
    # Coordinators pin the weights by digest instead of rehashing them.
    scoring += [
        "--model-sha256",
        identity["model"]["sha256"],
        "--model-bytes",
        str(identity["model"]["bytes"]),
    ]
    cache = Path(args.cache).resolve(strict=True)
    if not cache.is_dir():
        raise ValueError("cache must name an existing shared filesystem directory")
    if not re.fullmatch(r"[0-9a-f]{40}", args.execution_commit):
        raise ValueError("execution_commit must be a full Git commit SHA")
    assets = Path(args.assets_root).absolute()
    if assets != Path("/var/lib/llm-cc"):
        raise ValueError(
            "Bazzite assets root must be /var/lib/llm-cc for the shared GPU lock"
        )
    bundle, checksum = package_bundle(args.source_root)
    bundle_path = assets / "packages" / (checksum + ".zip")
    links = json.loads(args.report_links) if args.report_links else {}
    validate_report_links(links)
    generation_id = hashlib.sha256(
        canonical_bytes(
            [
                identity,
                scoring,
                host,
                checksum,
                str(llm_cc),
                str(model),
                str(cache),
                args.endpoint,
                args.execution_repository,
                args.execution_commit,
                links,
            ]
        )
    ).hexdigest()
    generation = assets / "configurations" / generation_id
    config = {
        # The coordinator plans, and the worker scores and aggregates, with
        # this one pinned llm-cc; a plan from another build is refused.
        "llm_cc": str(llm_cc),
        "scoring_args": str(generation / "scoring.args"),
        "execution_host": str(generation / "execution-host.json"),
        "model": str(model),
        "cache": str(cache),
        "endpoint": args.endpoint,
        "api_key_env": "BUILDBUDDY_API_KEY",
        "github_token_env": "GITHUB_TOKEN",
        "pool": "linux-amd64-rocm",
        "execution_repository": args.execution_repository,
        "execution_commit": args.execution_commit,
        "execution_image": "none",
        "execution_bundle": str(bundle_path),
        "execution_bundle_sha256": checksum,
        "worker_secret_env": [],
        "worker_env": {},
        "platform_properties": {"EstimatedComputeUnits": "1"},
        "max_workers": 1,
        "cache_concurrency": 8,
        "refresh_days": 20,
        "expire_days": 30,
        "report_links": links,
    }
    provision_lock(assets, host["resource_id"], args.lock_group)
    # Write the executable package and identities before exposing their config.
    atomic_publish(bundle_path, bundle)
    atomic_publish(
        generation / "scoring.args", "".join(line + "\n" for line in scoring).encode()
    )
    atomic_publish(generation / "execution-host.json", canonical_bytes(host) + b"\n")
    atomic_publish(generation / "identity.json", canonical_bytes(identity) + b"\n")
    atomic_publish(generation / "comparison.json", canonical_bytes(config) + b"\n")
    output = Path(args.output) if args.output else assets / "comparison.json"
    atomic_publish(output, canonical_bytes(config) + b"\n")
    # The launcher is the consumer-facing entry point; publish it only once its
    # pinned package and immutable generation are already on disk.
    atomic_publish(
        assets / "bin" / "llm-cc-coordinate",
        launcher_script(bundle_path, checksum, generation / "comparison.json"),
        0o755,
    )
    return output


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets-root", default="/var/lib/llm-cc")
    parser.add_argument(
        "--source-root", default=str(Path(__file__).resolve().parents[2])
    )
    parser.add_argument("--installed-root", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument(
        "--execution-host",
        required=True,
        help="JSON AMD GPU identity, lock resource and runtime checksums",
    )
    parser.add_argument(
        "--scoring-args",
        help="scoring options, one argument per line (default: the ROCm "
        "dogfood contract)",
    )
    parser.add_argument("--cache", required=True)
    parser.add_argument("--execution-commit", required=True)
    parser.add_argument(
        "--execution-repository", default="https://github.com/pawelchcki/llm-cc.git"
    )
    parser.add_argument("--endpoint", default="https://pawel.buildbuddy.io")
    parser.add_argument("--output")
    parser.add_argument(
        "--report-links",
        help="JSON object of https URL templates published in PR comments, "
        "for example {\"baseline\": \"https://example/{repository}/baseline.md\"}",
    )
    parser.add_argument(
        "--lock-group",
        help="existing group shared by both executor users (default root)",
    )
    args = parser.parse_args(argv)
    print(configure(args))


if __name__ == "__main__":
    main()
