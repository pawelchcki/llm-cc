#!/usr/bin/env python3
"""Validate global entropy caching against a retained model-selection corpus.

Uses a new output directory, real inference, two independent Git repositories,
a linked worktree, and a non-Git copy. The retained study is only read.
"""

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time


def git(directory, *arguments):
    subprocess.run(["git", "-C", str(directory), *arguments], check=True,
                   stdout=subprocess.DEVNULL)


def initialize_repository(directory):
    git(directory, "init", "-q")
    git(directory, "add", ".")
    git(directory, "-c", "user.name=Cache validation", "-c",
        "user.email=cache-validation@example.invalid", "-c", "commit.gpgsign=false",
        "commit", "-qm", "Validation corpus")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--study", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    study, binary, output = (p.resolve() for p in
                             (args.study, args.binary, args.output))
    output.mkdir(parents=True, exist_ok=False)
    rows = json.loads((study / "corpus.json").read_text())["files"]
    names = [row["id"] for row in rows]
    repository = output / "repository"
    for row in rows:
        source = study / "corpus" / row["id"]
        assert hashlib.sha256(source.read_bytes()).hexdigest() == row["sha256"]
        target = repository / row["id"]
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    initialize_repository(repository)
    worktree = output / "worktree"
    git(repository, "worktree", "add", "-q", "--detach", str(worktree))
    other_repository = output / "other-repository"
    nongit = output / "nongit"
    for target in (other_repository, nongit):
        shutil.copytree(repository, target, ignore=shutil.ignore_patterns(".git"))
    initialize_repository(other_repository)
    entropy = output / "entropy"
    environment = os.environ | {"LLM_CC_ENTROPY_CACHE_DIR": str(entropy),
                               "CUDA_VISIBLE_DEVICES": "0"}
    summary = {"binary": str(binary), "runs": []}

    def run(model, label, source_root, expect_hit, extra=(), model_path=None):
        command = [str(binary), *[str(source_root / name) for name in names],
                   "--model", str(model_path or study / "models" / model["file"]),
                   "--no-download", "--gpu-layers", "-1", "--backend", "cuda",
                   "--context", "32768", "--batch-size", "256",
                   "--entropy-reduction", "device", "--hotspots", "10",
                   "--format", "jsonl", "--progress", "never", *extra]
        start = time.monotonic()
        process = subprocess.run(command, env=environment, capture_output=True,
                                 text=True, timeout=1200)
        elapsed = time.monotonic() - start
        prefix = output / (model["name"] + "--" + label)
        Path(str(prefix) + ".stderr").write_text(process.stderr)
        with gzip.open(str(prefix) + ".jsonl.gz", "wt") as stream:
            stream.write(process.stdout)
        assert process.returncode == 0, (label, process.stderr[-4000:])
        events = [json.loads(line) for line in process.stdout.splitlines()]
        files = [event for event in events if event["type"] == "file"]
        totals = [event for event in events if event["type"] == "totals"]
        assert len(files) == len(names) == 26
        assert len(totals) == 1 and not totals[0]["partial"]
        assert events[-1]["type"] == "totals"
        assert all(event["entropy_cache_hit"] == expect_hit for event in files)
        by_name = {str(Path(event["path"]).relative_to(source_root)):
                   {key: value for key, value in event.items()
                    if key not in ("path", "entropy_cache_hit")} for event in files}
        assert set(by_name) == set(names)
        record = {"model": model["name"], "label": label,
                  "wall_seconds": elapsed, "files": len(files),
                  "hits": sum(event["entropy_cache_hit"] for event in files),
                  "command": command, "totals": totals[0]}
        summary["runs"].append(record)
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(f"{model['name']} {label}: {elapsed:.3f}s, {record['hits']}/26 hits",
              flush=True)
        return by_name

    models = json.loads((study / "models.json").read_text())
    for model in models:
        if model["name"] not in ("qwen-1.5b-q6", "qwen-3b-q6"):
            continue
        baseline = run(model, "uncached", repository, False, ("--no-cache",))
        assert run(model, "populate", repository, False) == baseline
        for label, source_root in (("warm-1", repository), ("warm-2", repository),
                                   ("worktree", worktree),
                                   ("cross-repository", other_repository),
                                   ("nongit", nongit)):
            assert run(model, label, source_root, True) == baseline, label
        # Only this newly created test cache is modified. Preserve entropy to
        # isolate a first model hash from inference and compare it with warm hits.
        shutil.rmtree(entropy / "model-digests")
        assert run(model, "first-hash-existing-entropy", nongit, True) == baseline
        run(model, "threshold-alpha-hierarchy", nongit, True,
            ("--tau", "1.0", "--alpha", "0.5", "--hierarchy", "reference"))
        copied = output / model["file"]
        shutil.copyfile(study / "models" / model["file"], copied)
        assert run(model, "copied-model", nongit, True, model_path=copied) == baseline
        copied.unlink()
    status = subprocess.check_output(
        [str(binary), "cache", "status", "--format", "json"], env=environment,
        text=True)
    summary["status"] = json.loads(status)
    summary["valid"] = True
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
