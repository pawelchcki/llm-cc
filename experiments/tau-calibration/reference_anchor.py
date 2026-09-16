#!/usr/bin/env python3
"""Run the authors' Python pipeline with CodeLlama-7b-hf on the same corpus.

This anchors the calibration: the pooled 67th percentile of these entropies is
the quantity the paper reports as tau = 0.67. It also records the reference
raw LM-CC for each program for a scale comparison with llm-cc.
"""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

CACHE = Path.home() / ".cache" / "llm-cc-validation"
HF_REVISION = "6c284d1468fe6c413cf56183e69b194dcfa27fe6"
MODEL_SUFFIXES = {".bin", ".json", ".model", ".safetensors"}


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_head(path):
    """The checkout's commit when `path` is the root of a git work tree."""
    result = subprocess.run(["git", "-C", str(path), "rev-parse", "--show-toplevel", "HEAD"],
                            capture_output=True, text=True)
    if result.returncode != 0:
        return None
    top, head = result.stdout.split()
    return head if Path(top).resolve() == path.resolve() else None


def model_provenance(path):
    """Verify the checkpoint revision where it is knowable and hash its files."""
    path = path.resolve()
    revision = git_head(path) or (path.name if path.parent.name == "snapshots" else None)
    if revision is not None and revision != HF_REVISION:
        raise SystemExit(f"{path} is revision {revision}, expected {HF_REVISION}")
    files = {file.name: sha256_file(file) for file in sorted(path.iterdir())
             if file.is_file() and file.suffix in MODEL_SUFFIXES}
    if not files:
        raise SystemExit(f"no model files found in {path}")
    # `revision` is null when the directory carries no revision; the file
    # hashes are then the record of which weights produced the results.
    return dict(model="codellama/CodeLlama-7b-hf", expected_revision=HF_REVISION,
                revision=revision, files=files)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, default=CACHE / "lm-cc")
    parser.add_argument("--hf-model", type=Path, default=CACHE / "hf" / "CodeLlama-7b-hf")
    parser.add_argument("--work", type=Path, default=CACHE / "tau-calibration")
    parser.add_argument("--dtype", choices=["float32", "float16"], default="float32")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent)
    args = parser.parse_args()
    import torch  # Imported late so --help works without the ML stack.
    provenance = model_provenance(args.hf_model)
    manifest = json.loads((args.root / "corpus.json").read_text())
    inputs = dict(model=provenance, dtype=args.dtype,
                  reference_commit=git_head(args.repository),
                  corpus_sha256=hashlib.sha256(
                      json.dumps(manifest["files"], sort_keys=True).encode()).hexdigest())
    # The reference code is imported from this worktree; it must be the exact
    # commit the corpus was prepared from, without local edits.
    if inputs["reference_commit"] != manifest["revision"]:
        raise SystemExit(f"{args.repository} is at {inputs['reference_commit']}, "
                         f"expected {manifest['revision']}; run prepare.py")
    changes = subprocess.check_output(
        ["git", "-C", str(args.repository), "status", "--porcelain"], text=True)
    if changes.strip():
        raise SystemExit(f"{args.repository} has local changes; use a clean checkout:\n{changes}")
    os.chdir(args.repository / "scripts")
    sys.path.insert(0, str(args.repository / "scripts"))
    from lm_cc.lm_cc import (CodeBlockProcessor, TokenEntropyCalculator,
                             get_code_with_boundaries, get_lmcc)
    torch.set_num_threads(os.cpu_count())
    calculator = TokenEntropyCalculator(model_name=str(args.hf_model), device_map="cpu",
                                        float_type=getattr(torch, args.dtype))
    processor = CodeBlockProcessor()
    # Checkpoint after every program so an interrupted CPU run can resume. A
    # checkpoint is reused only for the exact model, corpus, and reference code.
    checkpoint = args.work / f"reference-anchor-{args.dtype}.partial.json"
    entropies, scores, started = {}, {}, time.monotonic()
    if checkpoint.exists():
        partial = json.loads(checkpoint.read_text())
        if partial.get("inputs") != inputs:
            raise SystemExit(f"{checkpoint} was produced from different inputs; delete it")
        entropies, scores = partial["entropies"], partial["llm_cc"]
    for index, row in enumerate(manifest["files"], 1):
        if row["id"] in scores:
            continue
        data = (args.work / "corpus" / f"{row['id']}.py").read_bytes()
        if hashlib.sha256(data).hexdigest() != row["sha256"]:
            raise RuntimeError(f"corpus checksum mismatch: {row['id']}")
        tokens, values = calculator.get_question_entropy(data.decode())
        values = [float(value) for value in values]
        text, _, spans = get_code_with_boundaries(tokens, values, threshold=0.67)
        tree = processor.parse_code_blocks(text, tokens=tokens, start_end_tokens=spans)
        entropies[row["id"]] = values
        scores[row["id"]] = get_lmcc(tree)
        # Replace atomically: a partial write would strand every completed
        # program behind a JSON parse error on the next run.
        partial_path = checkpoint.with_suffix(".writing")
        partial_path.write_text(json.dumps(dict(inputs=inputs, entropies=entropies,
                                                llm_cc=scores)))
        partial_path.replace(checkpoint)
        print(f"reference {index}/{len(manifest['files'])} {row['id']} "
              f"LM-CC={scores[row['id']]:.1f}", flush=True)
    (args.root / "results").mkdir(exist_ok=True)
    with gzip.open(args.root / "results" / "reference-codellama-7b-hf.entropy.json.gz",
                   "wt") as stream:
        json.dump(dict(**provenance, dtype=args.dtype, torch=torch.__version__,
                       reference_commit=inputs["reference_commit"],
                       corpus_sha256=inputs["corpus_sha256"],
                       wall_seconds=time.monotonic() - started,
                       entropies=entropies, llm_cc=scores), stream)


if __name__ == "__main__":
    main()
