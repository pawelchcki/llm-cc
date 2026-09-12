"""Validate existing host assets and publish a reproducible dogfood configuration.

This command does not build the scorer, download models, or provision a store.
Run it after installing the pinned scorer and materializing its ROCm profile.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import stat
import tempfile
import zipfile

from .common import canonical_bytes
from .profile import digest_file


def package_bundle(source_root):
    """Capture only the Python implementation, never untracked host credentials."""
    root = Path(source_root).resolve(strict=True)
    package = root / "tools" / "comparison"
    paths = [root / "tools" / "__init__.py"]
    paths.extend(
        sorted(p for p in package.glob("*.py") if not p.name.startswith("test_"))
    )
    paths.append(package / "dogfood-rules.json")
    required = {
        "__init__.py",
        "__main__.py",
        "buildbuddy.py",
        "worker.py",
        "pipeline.py",
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


def atomic_publish(path, payload):
    """Publish complete bytes, preserving any concurrent reader's prior file."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=".publish-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fchmod(stream.fileno(), 0o644)
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        Path(temporary).unlink(missing_ok=True)


def verify_assets(profile, installed_root, model):
    root = Path(installed_root).resolve(strict=True)
    build = profile["build"]
    host = build.get("execution_host", {})
    if build.get("execution_image") != "none" or host.get("gpu_arch") != "gfx1100":
        raise ValueError("Bazzite setup requires a bare-host gfx1100 scoring profile")
    if not host.get("runtime_files"):
        raise ValueError("bare-host runtime checksums are required")
    for path, checksum in host["runtime_files"].items():
        runtime = Path(path)
        if (
            not runtime.is_absolute()
            or not runtime.is_file()
            or digest_file(runtime) != checksum
        ):
            raise ValueError(f"runtime file does not match profile: {path}")
    files = build.get("installed_files", {})
    if "bin/llm-cc" not in files or not os.access(root / "bin/llm-cc", os.X_OK):
        raise ValueError("profile must include an executable installed bin/llm-cc")
    for relative, expected in files.items():
        candidate = root / relative
        if candidate.is_symlink() or root not in candidate.resolve().parents:
            raise ValueError(
                "installed scorer identity escapes its root or uses symlinks"
            )
        if not candidate.is_file() or digest_file(candidate) != expected:
            raise ValueError(f"installed file does not match profile: {relative}")
    model = Path(model).resolve(strict=True)
    if (
        not model.is_file()
        or model.stat().st_size != build.get("model_bytes")
        or digest_file(model) != build.get("model_sha256")
    ):
        raise ValueError("model size or checksum does not match profile")
    return root, model


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
    profile = json.loads(Path(args.profile).read_text(encoding="utf-8"))
    installed_root, model = verify_assets(profile, args.installed_root, args.model)
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
    rules = json.loads(
        (Path(args.source_root) / "tools/comparison/dogfood-rules.json").read_text()
    )
    identity = hashlib.sha256(
        canonical_bytes(
            [
                profile,
                rules,
                checksum,
                str(installed_root),
                str(model),
                str(cache),
                args.endpoint,
                args.execution_repository,
                args.execution_commit,
            ]
        )
    ).hexdigest()
    generation = assets / "configurations" / identity
    config = {
        "profile": str(generation / "profile.json"),
        "rules": str(generation / "rules.json"),
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
        "scorer": str(installed_root / "bin/llm-cc"),
        "installed_root": str(installed_root),
        "model": str(model),
        "worker_secret_env": [],
        "worker_env": {},
        "platform_properties": {"EstimatedComputeUnits": "1"},
        "max_workers": 1,
        "refresh_days": 20,
        "expire_days": 30,
    }
    provision_lock(
        assets, profile["build"]["execution_host"]["resource_id"], args.lock_group
    )
    # Write the executable package and identities before exposing their config.
    atomic_publish(bundle_path, bundle)
    atomic_publish(generation / "profile.json", canonical_bytes(profile) + b"\n")
    atomic_publish(generation / "rules.json", canonical_bytes(rules) + b"\n")
    atomic_publish(generation / "comparison.json", canonical_bytes(config) + b"\n")
    output = Path(args.output) if args.output else assets / "comparison.json"
    atomic_publish(output, canonical_bytes(config) + b"\n")
    return output


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets-root", default="/var/lib/llm-cc")
    parser.add_argument(
        "--source-root", default=str(Path(__file__).resolve().parents[2])
    )
    parser.add_argument("--profile", required=True)
    parser.add_argument("--installed-root", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--cache", required=True)
    parser.add_argument("--execution-commit", required=True)
    parser.add_argument(
        "--execution-repository", default="https://github.com/pawelchcki/llm-cc.git"
    )
    parser.add_argument("--endpoint", default="https://pawel.buildbuddy.io")
    parser.add_argument("--output")
    parser.add_argument(
        "--lock-group",
        help="existing group shared by both executor users (default root)",
    )
    args = parser.parse_args(argv)
    print(configure(args))


if __name__ == "__main__":
    main()
