# Smaller entropy models for llm-cc

An exploratory comparison on 2026-09-05 using real, checksum-verified Hugging
Face GGUFs and `dd-trace-c` source history. This evaluates an **entropy model
for code scoring**, not an assistant that generates or edits code.

## Findings and recommendation

**Qwen2.5-Coder-3B Q6_K is the closest smaller substitute tested;
Qwen2.5-Coder-1.5B Q6_K is the more economical compromise.** Both are worth
trying. The 0.5B variants save more memory, but lose more agreement with the
current model and provide little additional speed on this GPU. The built-in
default remains DeepSeek: this small pilot supports opt-in use, with a fresh
score baseline for each model.

| Base model / quantization | GGUF size (GB) | Median seconds | Peak GPU (GiB) | File-rank correlation with DeepSeek | Top-five files retained |
| --- | ---: | ---: | ---: | ---: | ---: |
| DeepSeek-Coder-V2-Lite Q6_K | 14.07 | 42.87 | 17.55 | 1.000 | 5/5 |
| Qwen2.5-Coder-3B Q6_K | 2.54 | 24.58 | 3.32 | 0.930 | 5/5 |
| Qwen2.5-Coder-1.5B Q6_K | 1.27 | 22.00 | 2.06 | 0.901 | 4/5 |
| Qwen2.5-Coder-0.5B Q6_K | 0.51 | 19.99 | 1.19 | 0.864 | 4/5 |
| Qwen2.5-Coder-0.5B Q4_K_M | 0.40 | 20.26 | 1.09 | 0.847 | 4/5 |

The 1.5B package is about **11 times smaller on disk**, uses **8.5 times less
GPU memory**, and runs **1.95 times faster** than the default here. For 3B the
corresponding factors are **5.5**, **5.3**, and **1.74**. These are complete
26-file runs on one A10G, not single-token generation speed. DeepSeek uses a
mixture of experts (16B total, 2.4B active parameters), so weight-size ratios
do not predict speed ratios. See its official model card below.

All 15 inference runs and 25 cached setting sweeps completed successfully:
40 complete outputs, each covering the same 26 files. The normalized scores
were identical across the three repetitions of each model.

The correlations are estimates from only 18 files. Bootstrap 95% intervals
are broad and overlap: 3B **0.721–0.994**, 1.5B **0.658–0.992**, 0.5B Q6
**0.578–0.975**, and 0.5B Q4 **0.526–0.975**. The sample does not establish a
statistically decisive winner. Controlling for file bytes leaves the 3B and
1.5B correlations at 0.935 and 0.907. On 46 matched functions of at least 100
tokens, their rank correlations are 0.921 and 0.896. Top-five hotspot-line
overlap averages only 66% and 69%, respectively: similar file rankings do not
mean identical diagnostics.

To try the already-registered 1.5B model:

```sh
llm-cc src --model-name qwen2.5-coder-1.5b-q6_k --gpu-layers -1 --context 32768
```

The tested 3B model is also registered, with its download pinned to the
experiment's Hugging Face revision:

```sh
llm-cc src --model-name qwen2.5-coder-3b-q6_k --gpu-layers -1 --context 32768
```

Keep model and settings fixed when comparing revisions.

## What happened to the historical improvements?

Changes in normalized LM-CC after each historical edit (negative means lower):

| Test file | Corrected DeepSeek | Qwen 0.5B Q6 | Qwen 1.5B Q6 | Qwen 3B Q6 |
| --- | ---: | ---: | ---: | ---: |
| `test_go_pclntab.c` | -21.08% | -15.47% | -22.26% | -24.80% |
| `test_go_hook_engine.c` | -8.12% | -8.44% | -8.44% | -3.90% |
| `test_go_ctx.c` | +19.05% | +14.89% | +26.03% | +20.98% |
| `test_go_read.c` | +0.47% | -1.92% | -0.25% | -0.69% |

The old session reported reductions for all four files. After the scorer
correction, DeepSeek itself reverses the context-test result and gives almost
no change on the reader test. Those old numbers are not current regression
expectations. The context change is consistently unfavorable under the
current metric across all tested models; the reader change is too small and
model-sensitive to be a robust success criterion. The pclntab edit also adds
coverage, so these pairs are not independent human-labeled simplifications.
Exact before/after values, including raw LM-CC and entropy, are in
[`edit-deltas.csv`](edit-deltas.csv).

## Thresholds, tokenizers, and apparent agreement

Switching models changes the numeric scale considerably. On the unchanged
files, the median absolute relative difference from DeepSeek is 59% for
1.5B and 55% for 3B. The same corpus produces 40,100 scored DeepSeek tokens
and 30,457 Qwen tokens. Tokenization and entropy distributions both change,
so normalized scores and absolute cutoffs cannot simply be carried over.

Changing tau to 1.0 gives correlations of 0.938 (1.5B) and 0.961 (3B) against
DeepSeek **also at tau 1.0**. At the 80th percentile they reach 0.979 and
0.996, but the structural-only ablation also reaches 0.996 across models.
High agreement alone can therefore reward a setting that suppresses the
model's contribution. The structural-only ablation correlates just 0.583
with DeepSeek's own ordinary scores. No setting was promoted on the basis
of agreement alone.

Some small edit deltas flip sign under threshold or quantization changes.
For example, the reader edit changes from -1.92% at Qwen 0.5B Q6 / tau 0.67
to about +0.2% at tau 0.4; Q4 at tau 0.67 yields -4.00%. Avoid interpreting a
1–2% movement as a reliable improvement without checking sensitivity. A
file-relative percentile also largely fixes high-entropy token density by
construction, so that density is not an independent validation target.

## Question and interpretation

Can a much smaller model preserve useful rankings and refactoring signals at
lower memory cost? Agreement with DeepSeek is a compatibility measurement,
not ground-truth accuracy. A candidate can disagree with DeepSeek and be right;
this experiment does not measure downstream coding-task success or human
maintainability judgments.

More parameters do not guarantee a better complexity metric. Training on
relevant code, probability calibration, tokenizer, and context length all
matter. Broader world knowledge can help with unfamiliar APIs and domains,
but this use case principally needs a useful uncertainty distribution over
source code. A lower average entropy or lower LM-CC score is not itself evidence
that a model is better. Confident mistakes can have low entropy.

The [updated LM-CC paper](https://arxiv.org/html/2602.07882v2) directly evaluated
Qwen2.5-Coder-1.5B as an entropy model. It reported useful correlations with
downstream performance, with results varying by task and entropy model. Those
results support trying 1.5B; they do not validate this project's normalized
score, quantized weights, or the current C-heavy corpus.

## Method

- 26 whole files: four historical before/after pairs and 18 unchanged files.
  The latter span C tests and production code, C++, Go, and Python. They are
  held out from the four historical edits, not from model pretraining. There
  are no tracked CUDA sources in this pinned revision.
- `corpus.json` records paths, immutable Git commits, sizes, and SHA-256 hashes.
  Historical base: `11133357`; final: `a417106c`. The changes are `9238eb3c`
  (pclntab), `381fd648` (hook engine), and `a417106c` (context/read tests).
- The earlier session's scores predate the scoring correction merged in
  `80123eb`. All model comparisons here use the corrected implementation.
  `provenance.json` records that distinction and the matching source hashes.
- One NVIDIA A10G, full GPU offload, device entropy reduction, batch size 256,
  context limit 32,768, structural hierarchy, tau 0.67, alpha 0.8. Each
  invocation loads one model and analyzes the complete corpus sequentially.
- Three timed inference runs per model, with no entropy-cache hits. The first
  populates an isolated cache; the next two explicitly disable it. Times
  include loading, preprocessing, inference, and output, but exclude downloads.
  They are not cold-disk or CPU/Mac measurements.
- GPU memory is sampled every 0.5 seconds as device memory use above the
  pre-run idle value. The GPU was otherwise idle. This is an approximate
  device peak, not host RAM or a theoretical minimum. For the two 0.5B
  variants, only the third run has valid memory telemetry; the initial
  PID-based query did not work across the container namespace.
- Cached sweeps test tau 0.4, tau 1.0, the 80th percentile, reference hierarchy,
  and tau 100 (no entropy boundaries; structural-only ablation). Cache hits
  are mandatory for these sweeps, whose runtimes are not inference timings.
- Each result must have exit status zero, every expected file exactly once,
  a terminal non-partial totals event, and the expected cache state. Raw JSONL
  is retained compressed alongside compact JSON records and stderr logs.
- File-rank comparisons use only the 18 unchanged files. Spearman correlation
  uses average ranks for ties; 95% intervals are paired bootstrap percentiles
  over files (2,000 draws, fixed seed). Partial Spearman controls for original
  file bytes. Function correlations require at least 100 scored tokens in both
  models and are descriptive: functions within one file are not independent.
- No threshold was fitted to make a model win. The four previous edits were
  selected using an older DeepSeek scorer and are a biased, very small sample.

## Artifacts

- `models.json`: exact Hugging Face revisions, download URLs, byte sizes, and
  independently verified LFS SHA-256 hashes.
- `environment.json`, `provenance.json`: binary digest, GPU, dependency ABI,
  source hashes, and historical context. Each result also has its full command
  and resolved configuration.
- `results/`: measurements, compressed original JSONL, and stderr.
- `summary.json`, `file-scores.csv`, `edit-deltas.csv`: derived comparisons.

Model weights, generated source snapshots, and entropy caches are ignored by
Git. The source snapshots can be reconstructed from an existing `dd-trace-c`
checkout; they are not vendored into this repository.

## Reproduce

Use a Linux CUDA build with the corrected scorer. The experiment used an
existing universal binary; see provenance for its source/build caveat. Use a
fresh output directory to preserve the recorded experiment. Completed valid runs with identical
commands are resumable; a changed binary/environment is rejected.

```sh
python3 experiments/model-selection/prepare.py /path/to/dd-trace-c --output /tmp/llmcc-study
cp experiments/model-selection/models.json /tmp/llmcc-study/models.json
python3 experiments/model-selection/download.py --root /tmp/llmcc-study --connections 8
python3 experiments/model-selection/run.py --root /tmp/llmcc-study --binary /absolute/path/to/llm-cc --repetitions 3
python3 experiments/model-selection/summarize.py --root /tmp/llmcc-study
```

The runner uses only Python's standard library, `curl`, `git`, and
`nvidia-smi`; it does not load remote Python model code. Downloads resume;
large parallel transfers need temporary space for both chunks and assembly.
The small base models have a native 32,768-token context, versus the default
DeepSeek model's advertised 128K. That tradeoff matters for large source files.
See the official [Qwen 1.5B model card](https://huggingface.co/Qwen/Qwen2.5-Coder-1.5B)
and [DeepSeek model card](https://huggingface.co/deepseek-ai/DeepSeek-Coder-V2-Lite-Base).
