#!/usr/bin/env python3
"""Export the paper's HumanEval programs, normalized by the reference code.

Both llm-cc and the reference entropy calculator must see identical bytes, so
each program is passed through the authors' own `remove_comments_and_docstrings`
(which also drops blank lines) before anything is scored.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess

REVISION = "c38a26afdfc29ee517d734c6b677a4d6c65ec59b"
REPOSITORY = "https://github.com/xchen121/lm-cc.git"
CACHE = Path.home() / ".cache" / "llm-cc-validation"


def reference_normalizer(repository):
    for candidate in sorted(repository.rglob("utils.py")):
        if "def remove_comments_and_docstrings" in candidate.read_text():
            spec = importlib.util.spec_from_file_location("lm_cc_utils", candidate)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            return candidate.relative_to(repository), module.remove_comments_and_docstrings
    raise RuntimeError("reference remove_comments_and_docstrings not found")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, default=CACHE / "lm-cc")
    parser.add_argument("--work", type=Path, default=CACHE / "tau-calibration")
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parent)
    args = parser.parse_args()
    if not args.repository.exists():
        subprocess.run(["git", "clone", "-q", REPOSITORY, str(args.repository)], check=True)
    subprocess.run(["git", "-C", str(args.repository), "checkout", "-q", REVISION], check=True)
    head = subprocess.check_output(
        ["git", "-C", str(args.repository), "rev-parse", "HEAD"], text=True).strip()
    if head != REVISION:
        raise RuntimeError(f"expected {REVISION}, found {head}")
    # Checkout keeps local edits, and the normalizer is imported from this tree,
    # so only a pristine worktree may be attributed to REVISION.
    changes = subprocess.check_output(
        ["git", "-C", str(args.repository), "status", "--porcelain"], text=True)
    if changes.strip():
        raise SystemExit(f"{args.repository} has local changes; use a clean checkout:\n{changes}")
    normalizer_path, normalize = reference_normalizer(args.repository)
    corpus = args.work / "corpus"
    corpus.mkdir(parents=True, exist_ok=True)
    tasks = [path for path in (args.repository / "dataset" / "humaneval").iterdir()
             if path.is_dir() and "__" not in path.name and (path / "main.py").is_file()]
    tasks.sort(key=lambda path: int(re.search(r"(\d+)$", path.name).group(1)))
    records, skipped = [], []
    for task in tasks:
        code = normalize((task / "main.py").read_text())
        if not code.strip():
            skipped.append(task.name)
            continue
        data = code.encode()
        (corpus / f"{task.name}.py").write_bytes(data)
        records.append(dict(id=task.name, bytes=len(data),
                            sha256=hashlib.sha256(data).hexdigest()))
    manifest = dict(repository=REPOSITORY, revision=REVISION,
                    normalizer=str(normalizer_path), skipped_empty=skipped, files=records)
    (args.output / "corpus.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Exported {len(records)} programs ({len(skipped)} empty after normalization) "
          f"to {corpus}")


if __name__ == "__main__":
    main()
