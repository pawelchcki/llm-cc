#!/usr/bin/env python3
"""Stage and verify the complete GitHub release contract (stdlib only)."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess


PLATFORMS = (
    "linux-x86_64",
    "linux-arm64",
    "windows-x86_64",
    "macos-arm64",
    "macos-x86_64",
)
BACKENDS = ("cuda", "rocm")


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def executable_name(version, platform):
    suffix = ".exe" if platform == "windows-x86_64" else ""
    return f"llm-cc-{version}-{platform}{suffix}"


def backend_name(backend):
    return f"llm-cc-backend-{backend}-linux-x86_64"


def checksum(path):
    path.with_name(path.name + ".sha256").write_text(
        f"{digest(path)}  {path.name}\n", encoding="utf-8"
    )


def check_bundle(bundle, manifest, commit):
    recorded = bundle.with_name(bundle.name + ".sha256").read_text().split()
    if not recorded or recorded[0] != digest(bundle):
        raise ValueError(f"Checksum mismatch: {bundle}")
    metadata = json.loads(manifest.read_text())
    if metadata.get("git_sha") != commit:
        raise ValueError(f"Backend commit mismatch: {manifest}")
    return metadata


def stage_binary(args):
    source = Path(args.binary).resolve()
    actual = subprocess.check_output([str(source), "--version"], text=True).strip()
    if actual != f"llm-cc {args.version}":
        raise ValueError(f"Release version mismatch: {actual}")
    target = args.output / executable_name(args.version, args.platform)
    shutil.copyfile(source, target)
    target.chmod(0o755)
    checksum(target)


def stage_backend(args):
    stem = backend_name(args.backend)
    source = Path(args.bundle)
    metadata = check_bundle(source, Path(args.manifest), args.commit)
    # A matching CI build can have a development version; the commit is binding.
    metadata["version"] = args.version
    target = args.output / (stem + ".bundle")
    shutil.copyfile(source, target)
    checksum(target)
    (args.output / (stem + ".manifest.json")).write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
    )


def verify(args):
    expected = set()
    platforms = {}
    for platform in PLATFORMS:
        binary = executable_name(args.version, platform)
        expected.update((binary, binary + ".sha256"))
        platforms[platform] = {
            "executable": binary,
            "backends": list(BACKENDS) if platform == "linux-x86_64" else [],
            "metal": platform.startswith("macos-"),
        }
    for backend in BACKENDS:
        stem = backend_name(backend)
        expected.update((stem + ".bundle", stem + ".bundle.sha256", stem + ".manifest.json"))
        metadata = check_bundle(
            args.output / (stem + ".bundle"),
            args.output / (stem + ".manifest.json"),
            args.commit,
        )
        if metadata.get("version") != args.version:
            raise ValueError(f"Backend version mismatch: {stem}")
    actual = {path.name for path in args.output.iterdir()}
    if actual != expected:
        raise ValueError(f"Incomplete asset set: missing={expected - actual}, extra={actual - expected}")
    for name in sorted(expected):
        path = args.output / name
        if not path.is_file() or path.stat().st_size == 0:
            raise ValueError(f"Empty or invalid asset: {path}")
        if name.endswith(".sha256"):
            original = args.output / name.removesuffix(".sha256")
            if path.read_text().strip() != f"{digest(original)}  {original.name}":
                raise ValueError(f"Checksum mismatch: {original}")
    manifest = args.output / "release-manifest.json"
    manifest.write_text(json.dumps({
        "version": args.version,
        "git_sha": args.commit,
        "base_url": f"https://github.com/pawelchcki/llm-cc/releases/download/v{args.version}",
        "platforms": platforms,
    }, indent=2) + "\n", encoding="utf-8")
    expected.add(manifest.name)
    (args.output / "SHA256SUMS").write_text(
        "".join(f"{digest(args.output / name)}  {name}\n" for name in sorted(expected)),
        encoding="utf-8",
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("binary", "backend", "verify"))
    parser.add_argument("--version", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--platform", choices=PLATFORMS)
    parser.add_argument("--binary")
    parser.add_argument("--backend", choices=BACKENDS)
    parser.add_argument("--bundle")
    parser.add_argument("--manifest")
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.version):
        parser.error("version must be MAJOR.MINOR.PATCH")
    if not re.fullmatch(r"[0-9a-f]{40}", args.commit):
        parser.error("commit must be a full Git SHA")
    args.output.mkdir(parents=True, exist_ok=True)
    {"binary": stage_binary, "backend": stage_backend, "verify": verify}[args.command](args)


if __name__ == "__main__":
    main()
