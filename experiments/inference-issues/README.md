# Inference memory and large-file validation

This experiment is separate from `model-selection` so the historical results
remain immutable. It validates the changes for issues 29, 30, and 32 on the
24 GiB Radeon RX 7900 XTX and the 8 GiB RTX 4060 Laptop GPU.

No result is implied by this directory. A run is recorded as complete only
when the process exits zero, emits exactly one final non-partial `totals`
event, covers every manifest entry, and does not report CPU Flash Attention
placement. Explicit Flash Attention runs must also report a positive placement
count on the selected GPU. OOM, token-limit, and other incomplete runs remain
in the results with their exit code and classification.

## Corpus and models

`prepare.py` reconstructs a 26-file `llm-cc` corpus from the clean revision
`495004c5561e60e0b2b6855ae73507431d593067`, checking every source against the
byte count and SHA-256 in `repository-corpus.json`. This gives baseline and
candidate binaries identical, public, reproducible scoring inputs.
It also creates deterministic, byte-exact C fixtures at 50, 100, 206, 400, and
1,024 KiB. The 206 KiB fixture is the required large-file regression case.

Use the checksum-pinned DeepSeek Q6_K and Qwen 0.5B Q4_K_M entries in
`models.json`. The latter is the same 397,808,128-byte model used by
`//:model_smoke_test`.

```sh
python3 experiments/inference-issues/prepare.py \
  --repository /path/to/llm-cc --output /data/llmcc-inference
python3 experiments/model-selection/download.py \
  --root /data/llmcc-inference --connections 8 \
  --model qwen-0.5b-q4 --model deepseek-v2-lite-q6
```

If only the deterministic large-file regressions are needed, add
`--fixtures-only`. This omits the repository inputs from that preparation.

## Runs

Build and copy the matching executable and backend bundle to an isolated
directory on each machine. On Bluefin use
`/var/home/pawel/.cache/llm-cc-validation`, not `/tmp`.

```sh
python3 experiments/inference-issues/run.py \
  --root /data/llmcc-inference --binary /absolute/path/llm-cc \
  --backend-dir /absolute/path/backends \
  --model deepseek-v2-lite-q6 --backend rocm --repetitions 3

python3 experiments/inference-issues/run.py \
  --root /var/home/pawel/.cache/llm-cc-validation \
  --binary /absolute/path/llm-cc --model qwen-0.5b-q4 \
  --backend-dir /absolute/path/backends \
  --backend cuda --repetitions 3

python3 experiments/inference-issues/summarize.py --root /data/llmcc-inference
```

Every configuration is an uncached full-file run with other inference and
scoring options held constant:

| label | Flash Attention | K/V cache | gate comparison |
| --- | --- | --- | --- |
| `baseline` | auto | F16 | reference setting |
| `flash-f16` | on | F16 | Flash Attention vs baseline |
| `flash-q8` | on | Q8_0 | Q8 vs flash-f16 |
| `flash-q4` | on | Q4_0 | exploratory |

The harness preserves the command, binary/model/corpus hashes, configuration
events, host and GPU samples, stderr, phase lines, compressed JSONL, exit code,
and explicit failure class. Each record also carries a canonical invocation
fingerprint covering the executable, backend artifacts, selected corpus,
GPU and driver identity, settings, configurations, and repetition count. The
summary rejects mixed or unbound fingerprints. Run hashing separately with a
fresh runtime cache after warming the entropy entries; the model-hash completion
phase reports bytes and duration without changing the digest contract.

CUDA and ROCm builds must additionally run a direct ggml backend-ops test for
Flash Attention with `DKQ=192`, `DV=128`, equal Q/KV heads, short and remainder
batches, and F16/Q8_0/Q4_0 K/V data against the CPU reference. The diagnostic
trace must show `FLASH_ATTN_EXT` on the GPU. End-to-end CUDA validation uses
Qwen; the Radeon acceptance case uses the 206 KiB fixture with DeepSeek.

The pinned backend-ops source includes fifteen focused legacy-DeepSeek cases
(five batch widths times three K/V types). Build it with the matching backend,
place that backend library on its loader path, and restrict the run to Flash
Attention and the `hsk=192` parameter text:

```sh
bazel build --config=cuda @llama_cpp//:test-backend-ops \
  @llama_cpp//:libllm-cc-backend-cuda.so
bazel-bin/external/+http_archive+llama_cpp/test-backend-ops \
  test -o FLASH_ATTN_EXT -p 'hsk=192.*kv=127'
```

## Promotion gates

`summarize.py` evaluates file and eligible-function Spearman correlation,
deterministic bootstrap intervals, complete top-five retention, hotspot
overlap, relative score drift, repetition spread, and historical edit signs.
Q8 is promoted only if file and eligible-function rho are both at least 0.98,
all baseline top-five files remain present, and no edit whose baseline change
is at least 5% reverses sign. Repeated scores within each configuration must
also differ by no more than `1e-6`. The same test is applied independently to
`flash-f16`. If a run is missing or incomplete, the corresponding candidate
default is not promoted. The recorded Radeon runs passed both gates, promoting
Flash Attention `on` and Q8_0 K/V; Q4_0 remains an explicit large-context option.

The earlier A10G run measured 17,973 MiB above idle (17.55 GiB) for the
14,066,972,416-byte DeepSeek artifact at context 32,768 and batch 256. Record
new Radeon/Bluefin figures rather than extrapolating this value.
