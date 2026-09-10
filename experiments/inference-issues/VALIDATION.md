# Validation record

This record is separate from benchmark output so later benchmark runs can be
added without replacing historical results.

## Bluefin CUDA validation (2026-09-10)

- Host GPU: NVIDIA GeForce RTX 4060 Laptop GPU, 8188 MiB, compute capability
  8.9
- Driver: 610.57.04
- Initial validation `llm-cc` SHA-256:
  `1bd3a2f5a7fa5fed1614a0b2b05d5776a433502811cd3288c35cc5f778688deb`
- Initial CUDA backend SHA-256:
  `9df8f10c1595727dc0c3981a9bd14a1dd08baab0b020fcb0b4e29e2452f94281`
- Final integrated `llm-cc` SHA-256:
  `f9f14c2118fc6aa202c405f9aa7a5739f208d93399a2ac0c59d39effb5395637`
- Final integrated CUDA backend SHA-256:
  `e34b70d7c52705e8f420425c188f103ee0326f9f052d2d10beeba3771e8d2ab0`
- `test-backend-ops` SHA-256:
  `1be4fc0bfd955524cbf05e3de874c244b4d3f1cba87852c8a62f3b110e801568`
- Qwen model SHA-256:
  `fefe9d34df79d3ad3db33519d81112be76096626a5fdb87a98092035dc79ad68`

The direct backend operation suite ran on `CUDA0`, compared against the CPU
reference, and passed all 15 cases. It exercised the legacy DeepSeek shape
`DKQ=192`, `DV=128`, equal Q/KV head counts, K/V types F16, Q8_0, and Q4_0,
and prompt widths 1, 3, 17, 33, and 75 with `kv=127`.

The pinned Qwen model then completed end-to-end inference independently with
F16, Q8_0, and Q4_0 K/V caches. Each run used `--flash-attn on`, full CUDA
offload, device entropy reduction, and GPU K/V placement. Diagnostics reported
`FLASH_ATTN_EXT@CUDA0 count=24` for every configuration. The requested context
capacity was 64 tokens and llama.cpp allocated its padded capacity of 256.

After merging the final `origin/main` and promoting the gated defaults, the
matching artifacts were rebuilt and revalidated on Bluefin. The direct suite
again passed all 15 cases on `CUDA0`. A no-override Qwen analysis reported
requested and effective Flash Attention `on`, Q8_0 K/V, and GPU K/V offload,
then emitted a final non-partial total. llm-cc requested 799 context tokens,
llama.cpp allocated 1,024, and diagnostics reported
`FLASH_ATTN_EXT@CUDA0 count=96` with no CPU attention placement.

Artifacts and full logs are retained on Bluefin under
`/var/home/pawel/.cache/llm-cc-validation`, with the final integrated run in
`post-main-495004c`.

## Local build and test validation

- Uncached CPU unit/integration/model-smoke suite: 23 passed, one payload test
  skipped because its optional payload was not supplied.
- Post-promotion pinned Qwen model smoke: passed in 72.2 seconds, including
  context growth and reuse in both file orders and Q8_0 K/V inference.
- Uncached ROCm unit/integration suite: 22 passed, with the same optional
  payload test skipped.
- CUDA backend, CUDA executable, ROCm backend, and both direct-operation test
  targets: built successfully.
- Formatting, Python syntax, exact fixture sizing, generated-C syntax, shell
  syntax, and diff whitespace checks: passed.

The repository-wide configured clang-tidy run is not green: it reports
existing warnings-as-errors in untouched sources such as `cache_io.h` and
`sha256.h`, as well as generated `build_config.h`. The analysis was also run
on the changed source set; findings introduced in the new hashing and backend
diagnostic code were corrected without expanding this work into a repository
lint cleanup.

## Bazzite Radeon validation (2026-09-10)

- Checkout HEAD and `origin/main`:
  `495004c5561e60e0b2b6855ae73507431d593067`
- Host GPU: AMD Radeon RX 7900 XTX, `gfx1100`, 24,560 MiB
- Host kernel: `6.17.7-ba29.fc43.x86_64`
- Correlation-run binary SHA-256:
  `d95eb7e8bb7deb4f4e351657b2d8c3037d8be07dcdc61a0fd5213595c47b3e27`
- Final promoted-default binary SHA-256:
  `0b152664bf9b273bef8f1e03fc897b114816888bd9e08059648e9584c493d068`
- ROCm backend SHA-256:
  `d6846237cb0edf114d2236a2a45dbcec1aaceb231bd198299db2bbf95c3e8a0b`
- DeepSeek Q6_K model SHA-256:
  `5a2e25280075d769abdb111de8211d9d3367f2ae0d0e6166a288ee6e8ed0345d`

The direct backend operation suite ran on `ROCm0`, compared against the CPU
reference, and passed all 15 legacy DeepSeek cases. This covers `DKQ=192`,
`DV=128`, equal Q/KV head counts, F16/Q8_0/Q4_0 K/V data, and prompt widths 1,
3, 17, 33, and 75 with `kv=127`.

The self-contained correlation corpus contains 26 production files from clean
`llm-cc` revision `495004c5561e60e0b2b6855ae73507431d593067`, with every
byte count and SHA-256 pinned in `repository-corpus.json`. Its prepared
manifest SHA-256 is
`3718d617ffaab795c9846e3996ba88247f48502540a22f18d7aa210e4889d8fa`.
All 12 DeepSeek runs (four configurations times three uncached repetitions)
completed all 26 files and 83,120 scored tokens with final non-partial totals.

Explicit Flash Attention agrees exactly with the auto/F16 reference: file and
eligible-function Spearman rho are both 1.0 across 26 files and 182 functions,
all top-five files are retained, hotspot overlap is 1.0, and observed score
drift is zero. Q8_0 compared with Flash/F16 has file rho 0.98359, function rho
0.99208, complete top-five retention, hotspot overlap 0.92308, and median
relative score drift 0.00455. Repetitions within each configuration had zero
observed score variation. Both independent gates pass. Q4_0 fails its
exploratory gate with file rho 0.97265 and function rho 0.96724 despite 5/5
top-file retention. Earlier historical edit pairs were unavailable, so no
edit-direction claim is made.

Median wall times were 125.8 seconds for auto/F16, 107.3 for Flash/F16, 108.1
for Flash/Q8, and 107.9 for Flash/Q4. Peak sampled GPU use was 16,663-16,881
MiB for F16, 15,493 MiB for Q8, and 14,869 MiB for Q4. Full correlation
artifacts are retained under
`/var/home/pawel/.cache/llm-cc-correlation-495004c`.

The deterministic regression input is exactly 210,944 bytes and 6,206 lines
(SHA-256 `f22f72c91f12f9d089b0661842009ffcd6c04b2225c1dc76175d41ee194ddb78`).
It produced 74,445 scored tokens. With explicit Flash Attention, Q4_0 K/V,
full GPU offload, device entropy reduction, context limit 131,072, and batch
256, llama.cpp allocated a padded 74,496-token context, a 5,524.45 MiB GPU KV
buffer, and a 772.91 MiB GPU compute buffer. Diagnostics reported
`FLASH_ATTN_EXT@ROCm0 count=7857` and no CPU attention placement.

Three uncached repetitions completed with final non-partial totals in 182.1,
177.2, and 168.0 seconds. Peak GPU use was 20,232 MiB in every run, sampled
mean GPU utilization was 83.2%, 83.3%, and 86.0%, and peak host RSS was about
13.52 GiB. The file score was exactly repeatable at
`0.00014776009134260192` (zero observed drift).

The same input records explicit hardware limits. Automatic Flash Attention
with F16 and explicit Flash Attention with F16 both fail the 19,642.50 MiB KV
allocation. Q8_0 K/V reaches graph reservation but then fails its 772.91 MiB
GPU compute-buffer allocation. These are retained as `oom`, with nonzero exit
status and incomplete JSONL totals.

The passing independent correlation gates promote Flash Attention `on` and
Q8_0 K/V as defaults; Q4_0 remains opt-in. The large-file OOM measurements
remain important: users needing this 74K-token context on a 24 GiB card must
select Q4_0 explicitly. Full commands, JSONL, stderr, configurations, and
telemetry are retained under
`/var/home/pawel/.cache/llm-cc-radeon-validation`.

After promotion, an uncached no-override DeepSeek run completed on ROCm with a
final non-partial total. Its configuration reported requested and effective
Flash Attention `on`, Q8_0 K/V, GPU K/V offload, and device entropy reduction.
For the 1,039-token input, llm-cc requested a 1,040-token context and
llama.cpp allocated its padded 1,280-token capacity. Diagnostics reported
`FLASH_ATTN_EXT@ROCm0 count=135` and no CPU attention placement. The complete
CPU suite then passed 23 tests including the model smoke, and the complete
ROCm unit/integration suite passed 22 tests; each skipped only the optional
payload test. A final Radeon backend-operation run passed all 15 cases.

## Accelerated model hashing

The 14,066,972,416-byte DeepSeek artifact was measured with the entropy entry
warm and its digest memo moved aside in an isolated cache. Native SHA-256
completed in 13,510 ms at approximately 1.04 GB/s and reproduced the pinned
digest. The process completed from the warm entropy entry in 13.56 seconds
with 8,908 KiB peak RSS and did not load the model. A following run using the
validated private digest memo and the same entropy entry completed in 0.02
seconds with 8,612 KiB peak RSS. Byte heartbeats were emitted at five-second
intervals during the fresh hash.

Hashing artifacts are retained under
`/var/home/pawel/.cache/llm-cc-hash-validation`.
