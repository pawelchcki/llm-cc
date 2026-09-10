#!/usr/bin/env python3
"""Reconstruct checksum-pinned sources and deterministic large C fixtures."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

SIZES_KIB = (50, 100, 206, 400, 1024)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def fixture(size):
    target = size * 1024
    header = (b"/* deterministic llm-cc large-file fixture */\n"
              b"#include <stdint.h>\n"
              b"uint64_t mix(uint64_t x) {\n")
    statement = b"                x += UINT64_C(1);\n"
    footer = b"    return x;\n}\n"
    data = bytearray(header)
    while len(data) + len(statement) + len(footer) <= target:
        data.extend(statement)
    remaining = target - len(data) - len(footer)
    if remaining:
        if remaining < 4:
            data.extend(b" " * remaining)
        else:
            data.extend(b"/*" + b"x" * (remaining - 4) + b"*/")
    data.extend(footer)
    assert len(data) == target
    return bytes(data)


def source_bytes(repository, row):
    return subprocess.check_output(
        ["git", "-C", str(repository), "show", f"{row['commit']}:{row['source']}"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path)
    parser.add_argument(
        "--fixtures-only",
        action="store_true",
        help="prepare deterministic fixtures without fetching historical sources",
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not args.fixtures_only and args.repository is None:
        parser.error("--repository is required unless --fixtures-only is used")
    root = args.output.resolve()
    corpus = root / "corpus"
    corpus.mkdir(parents=True, exist_ok=True)
    benchmark = json.loads(
        (Path(__file__).parent / "repository-corpus.json").read_text())
    records = []
    if not args.fixtures_only:
        for row in benchmark["files"]:
            data = source_bytes(args.repository, row)
            if len(data) != row["bytes"] or digest(data) != row["sha256"]:
                raise RuntimeError(f"checksum mismatch: {row['id']}")
            target = corpus / row["id"]
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            records.append(row)
    for size in SIZES_KIB:
        data = fixture(size)
        identifier = f"fixtures/deterministic-{size}k.c"
        target = corpus / identifier
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        records.append({"id": identifier, "group": "fixture", "source": "generated",
                        "commit": "deterministic-v2", "bytes": len(data),
                        "sha256": digest(data)})
    subprocess.run(["git", "init", "-q", str(corpus)], check=True)
    manifest = {"repository": benchmark["repository"],
                "revision": benchmark["revision"], "files": records}
    (root / "corpus.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (root / "models.json").write_text((Path(__file__).parent / "models.json").read_text())
    print(f"prepared {len(records)} files, {sum(r['bytes'] for r in records):,} bytes")


if __name__ == "__main__":
    main()
