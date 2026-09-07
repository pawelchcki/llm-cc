#!/usr/bin/env python3
"""Download the matching llm-cc GitHub release executable, verifying SHA-256."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import tempfile
from urllib.request import Request, urlopen


REPOSITORY = "pawelchcki/llm-cc"


def target_platform(system=None, machine=None):
    system = (system or platform.system()).lower()
    machine = (machine or platform.machine()).lower()
    system = {"darwin": "macos", "windows": "windows", "linux": "linux"}.get(system)
    arch = {"amd64": "x86_64", "x86_64": "x86_64", "aarch64": "arm64", "arm64": "arm64"}.get(machine)
    if system is None or arch is None or (system == "windows" and arch != "x86_64"):
        raise ValueError("Supported systems: Linux/macOS x86-64 and ARM64, Windows x64")
    return f"{system}-{arch}"


def fetch(url):
    return urlopen(Request(url, headers={"User-Agent": "llm-cc-release-installer"}), timeout=60)


def install(version, destination):
    target = target_platform()
    if version == "latest":
        with fetch(f"https://api.github.com/repos/{REPOSITORY}/releases/latest") as response:
            version = json.load(response)["tag_name"]
    version = version.removeprefix("v")
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise ValueError("version must be MAJOR.MINOR.PATCH (or latest)")
    suffix = ".exe" if target.startswith("windows-") else ""
    asset = f"llm-cc-{version}-{target}{suffix}"
    base = f"https://github.com/{REPOSITORY}/releases/download/v{version}"
    with fetch(f"{base}/{asset}.sha256") as response:
        checksum = response.read(4096).decode("ascii").split()
    if len(checksum) != 2 or checksum[1] != asset or not re.fullmatch(r"[0-9a-f]{64}", checksum[0]):
        raise ValueError("Invalid release checksum file")
    destination.mkdir(parents=True, exist_ok=True)
    executable = destination / f"llm-cc{suffix}"
    # Keep the temporary download on the same filesystem for an atomic install.
    with tempfile.TemporaryDirectory(prefix=".llm-cc-", dir=destination) as temporary:
        downloaded = Path(temporary) / asset
        print(f"Downloading {base}/{asset}", flush=True)
        digest = hashlib.sha256()
        with fetch(f"{base}/{asset}") as response, downloaded.open("wb") as output:
            while block := response.read(1024 * 1024):
                digest.update(block)
                output.write(block)
        if digest.hexdigest() != checksum[0]:
            raise ValueError("Release download failed SHA-256 verification")
        downloaded.chmod(0o755)
        os.replace(downloaded, executable)
    print(f"Installed {executable}")
    return executable


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", default="latest")
    parser.add_argument("--bin-dir", type=Path, default=Path.home() / ".local" / "bin")
    args = parser.parse_args()
    try:
        install(args.version, args.bin_dir.expanduser().resolve())
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"Install failed: {error}\n")


if __name__ == "__main__":
    main()
