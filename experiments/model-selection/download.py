#!/usr/bin/env python3
"""Fetch public GGUFs, pin their HF revisions and verify LFS SHA-256 digests."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess
import shutil
import urllib.request

MODELS = [
    ("qwen-0.5b-q4", "bartowski/Qwen2.5-Coder-0.5B-GGUF", "Qwen2.5-Coder-0.5B-Q4_K_M.gguf"),
    ("qwen-0.5b-q6", "bartowski/Qwen2.5-Coder-0.5B-GGUF", "Qwen2.5-Coder-0.5B-Q6_K.gguf"),
    ("qwen-1.5b-q6", "QuantFactory/Qwen2.5-Coder-1.5B-GGUF", "Qwen2.5-Coder-1.5B.Q6_K.gguf"),
    ("qwen-3b-q6", "bartowski/Qwen2.5-Coder-3B-GGUF", "Qwen2.5-Coder-3B-Q6_K.gguf"),
    ("deepseek-v2-lite-q6", "bartowski/DeepSeek-Coder-V2-Lite-Base-GGUF", "DeepSeek-Coder-V2-Lite-Base-Q6_K.gguf"),
]

def fetch_ranges(url, target, size, connections):
    """Resume independent byte ranges; the full LFS digest verifies their assembly."""
    width = (size + connections - 1) // connections
    def fetch(index):
        start, end = index * width, min((index + 1) * width, size) - 1
        part = Path(str(target) + f".part-{index:02d}")
        if not part.exists() or part.stat().st_size != end - start + 1:
            subprocess.run(["curl", "-fsSL", "--retry", "3", "--range", f"{start}-{end}",
                            "-o", str(part), url], check=True)
        if part.stat().st_size != end - start + 1:
            raise RuntimeError(f"Server did not return the requested byte range: {part}")
        print(f"Downloaded range {index+1}/{connections}", flush=True)
        return part
    with ThreadPoolExecutor(max_workers=connections) as executor:
        parts = list(executor.map(fetch, range(connections)))
    with target.open("wb") as output:
        for part in parts:
            with part.open("rb") as stream:
                shutil.copyfileobj(stream, output, 8 * 1024 * 1024)
    return parts

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--connections", type=int, choices=range(1, 17), default=1)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent)
    args = parser.parse_args()
    root = args.root.resolve()
    (root / "models").mkdir(parents=True, exist_ok=True)
    records = []
    pinned = {row["name"]: row for row in json.loads((root / "models.json").read_text())} if (root / "models.json").exists() else {}
    for name, repo, filename in MODELS:
        revision = pinned.get(name, {}).get("revision", "main")
        with urllib.request.urlopen(f"https://huggingface.co/api/models/{repo}/revision/{revision}?blobs=true") as response:
            info = json.load(response)
        blob = next(x for x in info["siblings"] if x["rfilename"] == filename)
        digest = blob["lfs"]["sha256"]
        url = f"https://huggingface.co/{repo}/resolve/{info['sha']}/{filename}"
        target = root / "models" / filename
        downloaded = target
        parts = []
        if not target.exists():
            partial = target.with_suffix(".partial")
            if blob["lfs"]["size"] > 4_000_000_000 and args.connections > 1:
                parts = fetch_ranges(url, partial, blob["lfs"]["size"], args.connections)
            else:
                subprocess.run(["curl", "-fL", "--retry", "3", "-C", "-", "-o", str(partial), url], check=True)
            downloaded = partial
        actual = hashlib.sha256()
        with downloaded.open("rb") as stream:
            for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
                actual.update(chunk)
        if actual.hexdigest() != digest:
            raise RuntimeError(f"Checksum mismatch: {downloaded}")
        if downloaded != target:
            downloaded.rename(target)
        for part in parts:
            part.unlink()
        records.append(dict(name=name, repo=repo, revision=info["sha"], file=filename,
                            bytes=target.stat().st_size, sha256=digest, url=url))
        manifest = root / "models.json.partial"
        manifest.write_text(json.dumps(records, indent=2) + "\n")
        manifest.replace(root / "models.json")
        print(f"VERIFIED {name}: {target.stat().st_size:,} bytes", flush=True)

if __name__ == "__main__":
    main()
