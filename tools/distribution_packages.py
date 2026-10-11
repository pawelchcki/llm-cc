#!/usr/bin/env python3
"""Package verified release binaries for npm, pip and Homebrew (stdlib only)."""

import argparse
import base64
import csv
import gzip
import hashlib
import io
import json
from pathlib import Path
import re
import stat
import tarfile
import zipfile

from release_assets import PLATFORMS, digest, executable_name


ROOT = Path(__file__).resolve().parent.parent
REPOSITORY = "https://github.com/pawelchcki/llm-cc"
DESCRIPTION = "Find the code that is hardest for a language model to read"
# Match the ABI floors enforced by release.yml, including the macOS SDK floor.
TARGETS = {
    "linux-x86_64": ("linux", "x64", "manylinux_2_28_x86_64"),
    "linux-arm64": ("linux", "arm64", "manylinux_2_35_aarch64"),
    "windows-x86_64": ("win32", "x64", "win_amd64"),
    "macos-arm64": ("darwin", "arm64", "macosx_14_0_arm64"),
    "macos-x86_64": ("darwin", "x64", "macosx_14_0_x86_64"),
}


def npm_name(platform):
    system, arch, _ = TARGETS[platform]
    return f"@pawelchcki/llm-cc-{system}-{arch}"


def npm_archive(name, version):
    return f"{name.removeprefix('@').replace('/', '-')}-{version}.tgz"


def wheel_name(platform, version):
    return f"llm_cc-{version}-py3-none-{TARGETS[platform][2]}.whl"


def tarball(path, files):
    # Fix timestamps and ownership so retries produce identical artifacts.
    with path.open("wb") as output, gzip.GzipFile(fileobj=output, mode="wb", mtime=0, filename="") as zipped:
        with tarfile.open(fileobj=zipped, mode="w") as archive:
            for name, (data, mode) in sorted(files.items()):
                entry = tarfile.TarInfo(name)
                entry.size = len(data)
                entry.mode = mode
                archive.addfile(entry, io.BytesIO(data))


def npm_package(output, version, name, extra, files):
    metadata = {
        "name": name, "version": version, "description": DESCRIPTION,
        "license": "MIT", "homepage": REPOSITORY,
        "repository": {"type": "git", "url": f"git+{REPOSITORY}.git"},
        "publishConfig": {"access": "public"}, **extra,
    }
    files = {
        "package/package.json": (json.dumps(metadata, indent=2).encode() + b"\n", 0o644),
        "package/LICENSE": ((ROOT / "LICENSE").read_bytes(), 0o644),
        **{f"package/{key}": value for key, value in files.items()},
    }
    tarball(output / npm_archive(name, version), files)


def wheel(output, version, platform, binary):
    tag = f"py3-none-{TARGETS[platform][2]}"
    info = f"llm_cc-{version}.dist-info"
    filename = "llm-cc.exe" if platform.startswith("windows-") else "llm-cc"
    files = {
        f"llm_cc/{source.name}": (source.read_bytes(), 0o644)
        for source in sorted((ROOT / "dist/python/llm_cc").glob("*.py"))
    }
    files.update({
        f"llm_cc/bin/{filename}": (binary, 0o755),
        f"{info}/METADATA": ((
            f"Metadata-Version: 2.4\nName: llm-cc\nVersion: {version}\n"
            f"Summary: {DESCRIPTION}\nRequires-Python: >=3.9\n"
            f"Project-URL: Homepage, {REPOSITORY}\n"
            "License-Expression: MIT\nLicense-File: LICENSE\n\n"
        ).encode(), 0o644),
        f"{info}/WHEEL": ((
            "Wheel-Version: 1.0\nGenerator: llm-cc-release\n"
            f"Root-Is-Purelib: false\nTag: {tag}\n"
        ).encode(), 0o644),
        f"{info}/entry_points.txt": (b"[console_scripts]\nllm-cc = llm_cc:main\n", 0o644),
        f"{info}/licenses/LICENSE": ((ROOT / "LICENSE").read_bytes(), 0o644),
    })
    record = io.StringIO(newline="")
    writer = csv.writer(record, lineterminator="\n")
    for name, (data, _) in sorted(files.items()):
        checksum = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()
        writer.writerow((name, f"sha256={checksum}", len(data)))
    writer.writerow((f"{info}/RECORD", "", ""))
    files[f"{info}/RECORD"] = (record.getvalue().encode(), 0o644)
    with zipfile.ZipFile(output / wheel_name(platform, version), "w", zipfile.ZIP_DEFLATED) as archive:
        for name, (data, mode) in sorted(files.items()):
            entry = zipfile.ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0))
            entry.create_system = 3
            entry.external_attr = (stat.S_IFREG | mode) << 16
            entry.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(entry, data)


def formula(output, version):
    lines = [
        "class LlmCc < Formula", f'  desc "{DESCRIPTION}"',
        f'  homepage "{REPOSITORY}"', f'  version "{version}"', '  license "MIT"', "",
    ]
    for os_name in ("macos", "linux"):
        lines.append(f"  on_{os_name} do")
        if os_name == "macos":
            lines.append('    depends_on macos: ">= :sonoma"')
        for cpu, arch in (("arm", "arm64"), ("intel", "x86_64")):
            archive = output / f"llm-cc-{version}-{os_name}-{arch}.tar.gz"
            lines.extend([
                f"    on_{cpu} do",
                f'      url "{REPOSITORY}/releases/download/v{version}/{archive.name}"',
                f'      sha256 "{digest(archive)}"', "    end",
            ])
        lines.extend(["  end", ""])
    lines.extend([
        "  def install", '    bin.install "bin/llm-cc"', "  end", "",
        "  test do", '    assert_equal "llm-cc #{version}\\n", shell_output("#{bin}/llm-cc --version")',
        "  end", "end", "",
    ])
    (output / "llm-cc.rb").write_text("\n".join(lines), encoding="utf-8")


def build(assets, output, version, platforms=PLATFORMS):
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise ValueError("version must be MAJOR.MINOR.PATCH")
    output.mkdir(parents=True, exist_ok=True)
    npm_package(output, version, "llm-cc", {
        "bin": {"llm-cc": "cli.cjs"}, "engines": {"node": ">=18"},
        "optionalDependencies": {npm_name(target): version for target in PLATFORMS},
    }, {"cli.cjs": ((ROOT / "dist/npm/cli.cjs").read_bytes(), 0o755)})
    for platform in platforms:
        path = assets / executable_name(version, platform)
        if path.with_name(path.name + ".sha256").read_text().strip() != f"{digest(path)}  {path.name}":
            raise ValueError(f"Checksum mismatch: {path}")
        binary = path.read_bytes()
        system, arch, _ = TARGETS[platform]
        filename = "llm-cc.exe" if system == "win32" else "llm-cc"
        extra = {"os": [system], "cpu": [arch]}
        if system == "linux":
            extra["libc"] = ["glibc"]
        npm_package(output, version, npm_name(platform), extra, {f"bin/{filename}": (binary, 0o755)})
        wheel(output, version, platform, binary)
        if system != "win32":
            tarball(output / f"llm-cc-{version}-{platform}.tar.gz", {
                "bin/llm-cc": (binary, 0o755), "LICENSE": ((ROOT / "LICENSE").read_bytes(), 0o644),
            })
    if set(platforms) == set(PLATFORMS):
        formula(output, version)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--platform", choices=PLATFORMS)
    args = parser.parse_args()
    build(args.assets, args.output, args.version, (args.platform,) if args.platform else PLATFORMS)


if __name__ == "__main__":
    main()
