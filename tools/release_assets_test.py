#!/usr/bin/env python3
"""Release contract and installer regression tests; no network or GPU needed."""

import argparse
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import install_release
import release_assets as assets


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.args = argparse.Namespace(output=self.root, version="1.2.3", commit="a" * 40)

    def prepare(self):
        for platform in assets.PLATFORMS:
            path = self.root / assets.executable_name(self.args.version, platform)
            path.write_bytes(b"executable")
            assets.checksum(path)
        for backend in assets.BACKENDS:
            stem = assets.backend_name(backend)
            path = self.root / (stem + ".bundle")
            path.write_bytes(b"backend")
            assets.checksum(path)
            (self.root / (stem + ".manifest.json")).write_text(json.dumps({
                "git_sha": self.args.commit, "version": self.args.version,
            }))

    def test_complete_release(self):
        self.prepare()
        assets.verify(self.args)
        metadata = json.loads((self.root / "release-manifest.json").read_text())
        self.assertEqual(metadata["base_url"], "https://github.com/pawelchcki/llm-cc/releases/download/v1.2.3")
        self.assertEqual(len(metadata["platforms"]), 5)
        for line in (self.root / "SHA256SUMS").read_text().splitlines():
            digest, name = line.split()
            self.assertEqual(digest, assets.digest(self.root / name))

    def test_missing_platform_fails(self):
        self.prepare()
        (self.root / assets.executable_name("1.2.3", "linux-arm64")).unlink()
        with self.assertRaisesRegex(ValueError, "Incomplete asset set"):
            assets.verify(self.args)

    def test_corrupt_executable_fails(self):
        self.prepare()
        (self.root / assets.executable_name("1.2.3", "windows-x86_64")).write_bytes(b"corrupt")
        with self.assertRaisesRegex(ValueError, "Checksum mismatch"):
            assets.verify(self.args)

    def test_different_backend_commit_fails(self):
        self.prepare()
        manifest = self.root / (assets.backend_name("cuda") + ".manifest.json")
        manifest.write_text(json.dumps({"git_sha": "b" * 40, "version": "1.2.3"}))
        with self.assertRaisesRegex(ValueError, "Backend commit mismatch"):
            assets.verify(self.args)

    def test_installer_platform_selection(self):
        for system, machine, expected in (
            ("Linux", "x86_64", "linux-x86_64"),
            ("Linux", "aarch64", "linux-arm64"),
            ("Windows", "AMD64", "windows-x86_64"),
            ("Darwin", "arm64", "macos-arm64"),
            ("Darwin", "x86_64", "macos-x86_64"),
        ):
            self.assertEqual(install_release.target_platform(system, machine), expected)
        for system, machine in (("Linux", "i686"), ("Windows", "ARM64"), ("FreeBSD", "amd64")):
            with self.assertRaises(ValueError):
                install_release.target_platform(system, machine)

    def test_installer_download_and_corruption(self):
        binary = b"downloaded executable"
        name = "llm-cc-1.2.3-linux-arm64"
        base = "https://github.com/pawelchcki/llm-cc/releases/download/v1.2.3/"
        requests = []
        def fetch(url):
            requests.append(url)
            if url.endswith(".sha256"):
                return io.BytesIO(f"{hashlib.sha256(binary).hexdigest()}  {name}\n".encode())
            return io.BytesIO(binary)
        with patch.object(install_release, "target_platform", return_value="linux-arm64"), patch.object(install_release, "fetch", side_effect=fetch):
            result = install_release.install("v1.2.3", self.root)
            self.assertEqual(result.read_bytes(), binary)
            self.assertEqual(requests, [base + name + ".sha256", base + name])
            with patch.object(install_release, "fetch", side_effect=[
                io.BytesIO(f"{'0' * 64}  {name}\n".encode()), io.BytesIO(b"corrupt"),
            ]):
                with self.assertRaisesRegex(ValueError, "SHA-256"):
                    install_release.install("1.2.3", self.root)
            self.assertEqual(result.read_bytes(), binary)


if __name__ == "__main__":
    unittest.main()
