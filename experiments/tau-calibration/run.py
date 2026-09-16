#!/usr/bin/env python3
"""Collect per-token entropies (and optionally raw LM-CC) for each model.

`entropy` runs `llm-cc score --entropy` once per program so every file gets a
fresh context, exactly like the reference calculator. `analysis` runs the
normal analysis over the corpus to record raw LM-CC under a given tau.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import time

CACHE = Path.home() / ".cache" / "llm-cc-validation"


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load(root, work, selected):
    manifest = json.loads((root / "corpus.json").read_text())
    for row in manifest["files"]:
        path = work / "corpus" / f"{row['id']}.py"
        if hashlib.sha256(path.read_bytes()).hexdigest() != row["sha256"]:
            raise RuntimeError(f"corpus checksum mismatch: {path}")
    models = json.loads((root / "models.json").read_text())
    unknown = set(selected or []) - {model["name"] for model in models}
    if unknown:
        raise SystemExit(f"unknown models: {', '.join(sorted(unknown))}")
    return manifest["files"], [m for m in models if not selected or m["name"] in selected]


def verified_model(work, model):
    path = work.parent / "models" / model["file"]
    if path.stat().st_size != model["bytes"] or sha256_file(path) != model["sha256"]:
        raise RuntimeError(f"model checksum mismatch: {path}")
    return path


def entropy_inputs(args, files, model):
    """Everything an entropy dump depends on; a dump is reused only on a match."""
    return dict(model_sha256=model["sha256"],
                corpus_sha256=hashlib.sha256(
                    json.dumps(files, sort_keys=True).encode()).hexdigest(),
                binary_version=subprocess.check_output([str(args.binary), "--version"],
                                                       text=True).strip(),
                # Unstamped development builds share one version string.
                binary_sha256=sha256_file(args.binary),
                inference=args.inference)


def entropy(args, root, files, model):
    target = root / "results" / f"{model['name']}.entropy.json.gz"
    inputs = entropy_inputs(args, files, model)
    if target.exists():
        with gzip.open(target, "rt") as stream:
            recorded = json.load(stream).get("inputs")
        if recorded != inputs:
            raise SystemExit(f"{target} was produced from different or unrecorded inputs "
                             f"(recorded {recorded}, current {inputs}); delete it to recompute")
        print(f"already complete: {target.name}", flush=True)
        return
    path = verified_model(args.work, model)
    rows, started = {}, time.monotonic()
    for index, row in enumerate(files, 1):
        source = args.work / "corpus" / f"{row['id']}.py"
        command = [str(args.binary), "score", "--model", str(path), "--file", str(source),
                   "--entropy", "--no-download", "--progress", "never", *args.inference]
        output = subprocess.run(command, check=True, capture_output=True, text=True).stdout
        records = [json.loads(line) for line in output.splitlines() if line.strip()]
        covered = b"".join(bytes.fromhex(r["bytes_hex"]) for r in records)
        # SentencePiece models (CodeLlama) report the tokenizer's dummy-prefix
        # space on the first piece, exactly like the reference `\u2581` token.
        if covered != source.read_bytes() and covered != b" " + source.read_bytes():
            raise RuntimeError(f"token bytes do not cover {source}")
        rows[row["id"]] = [r["entropy"] for r in records]
        print(f"{model['name']} {index}/{len(files)} {row['id']}", flush=True)
    record = dict(model=model["name"], sha256=model["sha256"], command_template=command[:1] +
                  ["score", "--model", model["file"], "--file", "PROGRAM", *command[6:]],
                  binary_version=inputs["binary_version"], inputs=inputs,
                  wall_seconds=time.monotonic() - started, entropies=rows)
    # Publish atomically: a truncated dump would be taken for a complete one on
    # the next run and fail to load instead of recomputing.
    partial_path = target.with_suffix(".writing")
    with gzip.open(partial_path, "wt") as stream:
        json.dump(record, stream)
    partial_path.replace(target)


def analysis(args, root, files, model):
    # default_tau in src/models.h is the matched-percentile value.
    tau = json.loads((root / "summary.json").read_text())["models"][model["name"]]["tau_matched"]
    inputs = entropy_inputs(args, files, model)
    target = root / "results" / f"{model['name']}.analysis.json"
    path = verified_model(args.work, model)
    sources = [str(args.work / "corpus" / f"{row['id']}.py") for row in files]
    command = [str(args.binary), *sources, "--model", str(path), "--no-download",
               "--progress", "never", "--hotspots", "0", "--tau", repr(tau), *args.inference]
    events = [json.loads(line) for line in subprocess.run(
        command, check=True, capture_output=True, text=True).stdout.splitlines()]
    totals = events[-1]
    if totals["type"] != "totals" or totals["partial"]:
        raise RuntimeError(f"incomplete analysis for {model['name']}")
    scores = {Path(e["path"]).stem: dict(llm_cc=e["llm_cc"], lmcc_per_token=e["lmcc_per_token"],
                                          tokens=e["token_count"])
              for e in events if e["type"] == "file"}
    payload = dict(model=model["name"], tau=tau, inputs=inputs, files=scores,
                   totals={k: totals[k] for k in ("llm_cc", "mean_llm_cc_per_file",
                                                  "token_count")})
    # Publish atomically: summarize.py parses every analysis artifact, so a
    # truncated one would block the next regeneration before it is rewritten.
    partial_path = target.with_suffix(".writing")
    partial_path.write_text(json.dumps(payload, indent=2) + "\n")
    partial_path.replace(target)
    print(f"{model['name']}: mean raw LM-CC per program {totals['mean_llm_cc_per_file']:.2f}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=["entropy", "analysis"])
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--model", action="append")
    parser.add_argument("--work", type=Path, default=CACHE / "tau-calibration")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--inference", default="",
                        help="extra llm-cc flags as one string, e.g. '--backend rocm'")
    args = parser.parse_args()
    args.inference = shlex.split(args.inference)
    # Later flags win in the CLI, so these would silently replace the verified
    # model or the calibrated tau that each artifact records.
    reserved = ("--model", "--model-name", "--tau", "--tau-percentile")
    if any(flag.split("=", 1)[0] in reserved for flag in args.inference):
        parser.error("--inference must not set " + ", ".join(reserved))
    root = args.root.resolve()
    args.binary = args.binary.resolve()
    (root / "results").mkdir(exist_ok=True)
    files, models = load(root, args.work, args.model)
    for model in models:
        (entropy if args.phase == "entropy" else analysis)(args, root, files, model)


if __name__ == "__main__":
    main()
