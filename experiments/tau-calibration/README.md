# Per-model entropy thresholds (τ) for llm-cc

LM-CC splits code into blocks where token entropy reaches a threshold τ. The
paper (*Rethinking Code Complexity Through the Lens of Large Language Models*,
arXiv 2602.07882) sets τ = 0.67 nats, described as the 67th percentile of
CodeLlama-7b's token entropies on its corpus. An absolute 0.67 is only
meaningful for that model, because every model has its own entropy
distribution. This experiment finds the entropy percentile that 0.67
represents for CodeLlama-7b, transfers that percentile to each model
registered in llm-cc, and records the resulting `default_tau` values in
`src/models.h`.

## Results

On the 162 programs, the authors' own pipeline (CodeLlama-7b-hf) reaches 0.67
nats at the **62.48th** percentile of its token entropies, not the 67th: its
plain 67th percentile here is 0.758. The paper's 67th percentile was taken over
its full corpus. Transferring the 62.48th percentile keeps CodeLlama at exactly
the paper's operating point, and gives every model the same share of boundary
tokens. `default_tau` is that matched value.

| Model | Tokens | 67th percentile | Matched τ (`default_tau`) | Tokens ≥ 0.67 | Mean raw LM-CC per program |
| --- | ---: | ---: | ---: | ---: | ---: |
| CodeLlama-7b-hf, reference pipeline (float16) | 13,197 | 0.758 | 0.670 | 37.5% | 9.31 (reference hierarchy) |
| `codellama-7b-q8_0` | 13,197 | 0.760 | 0.676 | 37.7% | not run |
| `deepseek-coder-v2-lite-base-q6_k` (default) | 12,972 | 0.725 | 0.586 | 34.9% | 12.17 |
| `deepseek-coder-6.7b-base-q6_k` | 14,515 | 0.948 | 0.797 | 42.2% | 12.99 |
| `qwen2.5-coder-3b-q6_k` | 10,529 | 0.928 | 0.778 | 41.9% | 12.07 |
| `qwen2.5-coder-1.5b-q6_k` | 10,529 | 0.938 | 0.789 | 42.1% | 11.94 |
| `qwen2.5-coder-0.5b-q4_k_m` | 10,529 | 1.165 | 0.994 | 49.5% | 11.96 |

- **Tool side matches the reference.** llm-cc's CodeLlama Q8_0 entropies give
  a matched τ of 0.676, within 0.006 of the authors' float16 pipeline on the
  same tokens.
- **A fixed 0.67 was wrong for every other model.** It marks between 35% and
  50% of tokens as high-entropy instead of the rule's 37.5%. The default model
  needs a lower threshold; the smaller models need higher ones.
- **Raw LM-CC is on the paper's scale.** With calibrated τ and the default
  structural hierarchy, llm-cc averages about 12 per HumanEval program for
  every model. The authors' pipeline averages 9.3 with CodeLlama. The gap comes
  from the deliberate structural boundaries and tree-sitter nesting, not from
  the threshold.
- **One file spot check.** On HumanEval_0 with CodeLlama Q8_0, reference
  hierarchy, and τ 0.67, llm-cc scores 13.0; the authors' pipeline scores 11.6.

The anchor ran in float16 on CPU; the float32 run exceeded the shared host's
memory. llm-cc entropies came from ROCm for the DeepSeek and CodeLlama models
and from CPU for the Qwen models.

## Method

- **Corpus.** The 162 HumanEval programs in the authors' reference repository
  ([xchen121/lm-cc](https://github.com/xchen121/lm-cc) at `c38a26af`,
  `dataset/humaneval/HumanEval_N/main.py`, excluding the `__0`/`__1`
  transformation variants). Each program is normalized with the authors' own
  `remove_comments_and_docstrings`, which also drops blank lines, so llm-cc and
  the reference calculator see identical bytes. `corpus.json` records every
  file's SHA-256.
- **llm-cc entropies.** `run.py entropy` runs `llm-cc score --entropy` once per
  program, so each file gets a fresh context as in the reference. Entropies are
  full-vocabulary next-token entropies in nats at temperature 1. Tokens without
  a predecessor have no entropy and are excluded.
- **τ.** `summarize.py` pools every scored token of a model over the corpus.
  It reports the plain 67th percentile (`tau_p67`) and the matched value
  (`tau_matched`): the percentile at which the reference CodeLlama entropies
  reach 0.67, applied to this model. Both use the same linear interpolation as
  `llmcc::Percentile`. `default_tau` is `tau_matched`.
- **Reference anchor.** `reference_anchor.py` runs the authors'
  `TokenEntropyCalculator` with `codellama/CodeLlama-7b-hf` (revision
  `6c284d14`, CPU, float16; a float32 run exceeded the shared host's memory)
  on the same files and records its entropies and
  raw LM-CC per program. Its pooled 67th percentile checks that this corpus
  reproduces the paper's 0.67. The registered `codellama-7b-q8_0` GGUF, scored
  by llm-cc, checks that the tool side agrees with the reference for the same
  model. No F16 GGUF of the base model is published, so Q8_0 is used.
- **Scale check.** `run.py analysis` runs ordinary llm-cc analysis over the
  corpus with each model's calibrated τ and records raw LM-CC per program.

Models and checksums are pinned in `models.json`.

## Data

`summary.json` is the checked-in record `src/models.h` is derived from. The
per-model entropy dumps and analyses behind it live on this repository's
protected, append-only `data/tau-calibration` branch; the tag
`data/tau-calibration-v1` marks the commit these results were taken from. They are mounted
at `results/` as a submodule that is never fetched by default:

```sh
git submodule update --init --checkout experiments/tau-calibration/results
```

## Reproduce

```sh
LLM_CC_BINARY=bazel-bin/llm-cc LLM_CC_INFERENCE='--backend rocm' \
  experiments/tau-calibration/regenerate.sh --reference --publish
```

The script initializes `results/` and runs the steps below. Existing entropy
dumps are reused, so delete a file to recompute it. `--reference` also reruns
the authors' pipeline. `--publish` commits the new results to the data branch
and stages the updated submodule pointer and `summary.json`.

```sh
python3 prepare.py
python3 reference_anchor.py --dtype float16  # needs torch, transformers, tree_sitter
python3 run.py entropy --binary "$(command -v llm-cc)" --inference='--backend rocm'
python3 summarize.py
python3 run.py analysis --binary bazel-bin/llm-cc --inference='--backend rocm'
python3 summarize.py
```

Models are expected in `~/.cache/llm-cc-validation/models`, the corpus in
`~/.cache/llm-cc-validation/tau-calibration/corpus`, and the Hugging Face
checkpoint in `~/.cache/llm-cc-validation/hf/CodeLlama-7b-hf`.
