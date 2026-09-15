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
import sys
import time

CACHE = Path.home() / ".cache" / "llm-cc-validation"
HF_REVISION = "6c284d1468fe6c413cf56183e69b194dcfa27fe6"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, default=CACHE / "lm-cc")
    parser.add_argument("--hf-model", type=Path, default=CACHE / "hf" / "CodeLlama-7b-hf")
    parser.add_argument("--work", type=Path, default=CACHE / "tau-calibration")
    parser.add_argument("--dtype", choices=["float32", "float16"], default="float32")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent)
    args = parser.parse_args()
    import torch  # Imported late so --help works without the ML stack.
    os.chdir(args.repository / "scripts")
    sys.path.insert(0, str(args.repository / "scripts"))
    from lm_cc.lm_cc import (CodeBlockProcessor, TokenEntropyCalculator,
                             get_code_with_boundaries, get_lmcc)
    torch.set_num_threads(os.cpu_count())
    calculator = TokenEntropyCalculator(model_name=str(args.hf_model), device_map="cpu",
                                        float_type=getattr(torch, args.dtype))
    processor = CodeBlockProcessor()
    manifest = json.loads((args.root / "corpus.json").read_text())
    # Checkpoint after every program so an interrupted CPU run can resume.
    checkpoint = args.work / f"reference-anchor-{args.dtype}.partial.json"
    entropies, scores, started = {}, {}, time.monotonic()
    if checkpoint.exists():
        partial = json.loads(checkpoint.read_text())
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
        checkpoint.write_text(json.dumps(dict(entropies=entropies, llm_cc=scores)))
        print(f"reference {index}/{len(manifest['files'])} {row['id']} "
              f"LM-CC={scores[row['id']]:.1f}", flush=True)
    (args.root / "results").mkdir(exist_ok=True)
    with gzip.open(args.root / "results" / "reference-codellama-7b-hf.entropy.json.gz",
                   "wt") as stream:
        json.dump(dict(model="codellama/CodeLlama-7b-hf", revision=HF_REVISION,
                       dtype=args.dtype, torch=torch.__version__,
                       wall_seconds=time.monotonic() - started,
                       entropies=entropies, llm_cc=scores), stream)


if __name__ == "__main__":
    main()
