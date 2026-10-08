#!/usr/bin/env python3
"""Small package contract and installed-wrapper regression tests."""

import base64
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile

import distribution_packages as packages
from distribution_smoke import smoke
from install_release import target_platform
from release_assets import PLATFORMS, checksum, executable_name


VERSION = "1.2.3"


class PackageTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.assets = self.root / "assets"
        self.output = self.root / "packages"
        self.assets.mkdir()
        for platform in PLATFORMS:
            binary = self.assets / executable_name(VERSION, platform)
            binary.write_bytes(f"binary:{platform}".encode())
            checksum(binary)

    def build(self):
        packages.build(self.assets, self.output, VERSION)

    def test_npm_packages_match_platform_and_version(self):
        self.build()
        for platform in PLATFORMS:
            with tarfile.open(self.output / packages.npm_archive(packages.npm_name(platform), VERSION)) as archive:
                metadata = json.load(archive.extractfile("package/package.json"))
                system, arch, _ = packages.TARGETS[platform]
                self.assertEqual(metadata["os"], [system])
                self.assertEqual(metadata["cpu"], [arch])
                self.assertEqual(metadata["version"], VERSION)
                filename = "llm-cc.exe" if system == "win32" else "llm-cc"
                binary = archive.getmember(f"package/bin/{filename}")
                self.assertEqual(binary.mode, 0o755)
                self.assertEqual(archive.extractfile(binary).read(), f"binary:{platform}".encode())
        with tarfile.open(self.output / packages.npm_archive("llm-cc", VERSION)) as archive:
            metadata = json.load(archive.extractfile("package/package.json"))
            self.assertEqual(metadata["optionalDependencies"], {packages.npm_name(p): VERSION for p in PLATFORMS})
            self.assertNotIn("scripts", metadata)
            self.assertEqual(archive.getmember("package/cli.cjs").mode, 0o755)

    def test_wheel_tags_checksums_and_executable_permissions(self):
        self.build()
        for platform in PLATFORMS:
            with zipfile.ZipFile(self.output / packages.wheel_name(platform, VERSION)) as archive:
                info = f"llm_cc-{VERSION}.dist-info"
                wheel = archive.read(f"{info}/WHEEL").decode()
                self.assertIn(f"Tag: py3-none-{packages.TARGETS[platform][2]}", wheel)
                self.assertIn("Root-Is-Purelib: false", wheel)
                record = list(csv.reader(io.StringIO(archive.read(f"{info}/RECORD").decode())))
                self.assertEqual({row[0] for row in record}, set(archive.namelist()))
                for name, recorded, size in record:
                    if name.endswith("/RECORD"):
                        self.assertEqual((recorded, size), ("", ""))
                        continue
                    data = archive.read(name)
                    expected = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()
                    self.assertEqual(recorded, f"sha256={expected}")
                    self.assertEqual(int(size), len(data))
                filename = "llm-cc.exe" if platform.startswith("windows-") else "llm-cc"
                self.assertEqual(archive.getinfo(f"llm_cc/bin/{filename}").external_attr >> 16 & 0o777, 0o755)

    def test_brew_formula_uses_verified_archives(self):
        self.build()
        formula = (self.output / "llm-cc.rb").read_text()
        self.assertIn('bin.install "bin/llm-cc"', formula)
        self.assertIn('shell_output("#{bin}/llm-cc --version")', formula)
        for platform in PLATFORMS:
            if platform.startswith("windows-"):
                continue
            path = self.output / f"llm-cc-{VERSION}-{platform}.tar.gz"
            self.assertIn(f"/releases/download/v{VERSION}/{path.name}", formula)
            self.assertIn(packages.digest(path), formula)
            with tarfile.open(path) as archive:
                self.assertEqual(archive.getmember("bin/llm-cc").mode, 0o755)
                self.assertEqual(archive.extractfile("bin/llm-cc").read(), f"binary:{platform}".encode())

    def test_modified_binary_is_rejected(self):
        (self.assets / executable_name(VERSION, "linux-x86_64")).write_bytes(b"corrupt")
        with self.assertRaisesRegex(ValueError, "Checksum mismatch"):
            self.build()

    def test_pip_selects_each_platform_and_rejects_unsupported_abi(self):
        self.build()
        destination = self.root / "downloads"
        for platform in PLATFORMS:
            with self.subTest(platform=platform):
                shutil.rmtree(destination, ignore_errors=True)
                subprocess.run([sys.executable, "-m", "pip", "download", "--no-index", "--no-deps",
                                "--only-binary=:all:", "--find-links", str(self.output),
                                "--dest", str(destination), "--platform", packages.TARGETS[platform][2],
                                f"llm-cc=={VERSION}"], check=True, capture_output=True)
                self.assertEqual([p.name for p in destination.iterdir()], [packages.wheel_name(platform, VERSION)])
        for unsupported in ("manylinux_2_27_x86_64", "musllinux_1_2_x86_64", "macosx_13_0_arm64", "win_arm64"):
            with self.subTest(platform=unsupported):
                result = subprocess.run([sys.executable, "-m", "pip", "download", "--no-index", "--no-deps",
                                         "--only-binary=:all:", "--find-links", str(self.output),
                                         "--dest", str(destination), "--platform", unsupported,
                                         f"llm-cc=={VERSION}"], capture_output=True)
                self.assertNotEqual(result.returncode, 0)

    def test_package_build_is_reproducible(self):
        self.build()
        expected = {path.name: packages.digest(path) for path in self.output.iterdir()}
        self.build()
        self.assertEqual(expected, {path.name: packages.digest(path) for path in self.output.iterdir()})

    @unittest.skipIf(os.name == "nt", "Unix fixture; release CI uses the actual Windows executable")
    def test_installed_wrappers_forward_arguments_streams_environment_and_exit(self):
        if not shutil.which("npm") or not shutil.which("node"):
            self.fail("node and npm are required for installed-wrapper tests")
        binary = self.assets / executable_name(VERSION, target_platform())
        binary.write_text('''#!/usr/bin/env python3
import json, os, signal, sys
if sys.argv[1:] == ["--version"]:
    print("llm-cc 1.2.3")
elif sys.argv[1:2] == ["--wrapper-probe"]:
    print(json.dumps([sys.argv[1:], os.getcwd(), os.environ["LLM_CC_WRAPPER_TEST"], sys.stdin.read()]))
    print("native stderr", file=sys.stderr)
    sys.exit(23)
elif sys.argv[1:] == ["--help"]:
    print("native help")
elif sys.argv[1:] == ["--terminate"]:
    os.kill(os.getpid(), signal.SIGTERM)
else:
    print("native invalid option", file=sys.stderr)
    sys.exit(2)
''')
        binary.chmod(0o755)
        checksum(binary)
        self.build()
        smoke(self.output, VERSION, binary, fixture=True)

    @unittest.skipUnless(os.environ.get("LLM_CC_TEST_HOMEBREW") == "1", "enable real brew installs with LLM_CC_TEST_HOMEBREW=1")
    def test_homebrew_installs_the_prebuilt_binary(self):
        from homebrew_smoke import smoke as brew_smoke
        binary = self.assets / executable_name(VERSION, target_platform())
        binary.write_text('#!/usr/bin/env python3\nprint("llm-cc 1.2.3")\n')
        binary.chmod(0o755)
        checksum(binary)
        self.build()
        brew_smoke(self.output, VERSION)


if __name__ == "__main__":
    unittest.main()
