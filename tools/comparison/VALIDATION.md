# Implementation validation

Checked on Linux x86_64 on 2026-09-11. These results establish local correctness.
The Bazzite ROCm acceptance recorded below supplements these checks;
CUDA and generated-comment publication acceptance remain outstanding.

- Root CPU unit/integration, TLS and comparison Bazel targets: 24 passed; the
  universal-payload integration test was skipped under the CPU profile. Universal
  payload validation remains in the separate GPU validation path.
- Renamed external-module consumer, `BACKEND=cpu tools/check_consumer.sh --all`:
  installer, CLI, language, TLS and comparison tests passed. All four external
  stage launchers resolved their runfiles, backend-ops labels analyzed, and the
  installed CLI reported the configured version/provenance.
- Final affected CLI, C++ formatting and comparison targets passed after the
  external-module and lint fixes. Targeted clang-tidy on the three modified C++
  tests passed.
- All 38 comparison tests pass directly and through the Bazel launcher. They cover synthetic cold, warm and incremental execution;
  deterministic one/four-worker scheduling; corrupt cache recovery; storage
  failures; strict JSONL validation; cancellation and timeout handling; unusual
  paths; incomplete coverage; and mismatched pipeline artifacts.
- A filesystem-backed warm whole-repository comparison with **synthetic seeded
  metrics** completed in 0.0464 seconds with 109 unique cache hits and zero GPU
  workers. This measures the CPU/cache stages, not model inference or remote
  object-store latency.

The companion publisher passed all 419 ci-toolkit unit tests and the TypeScript
check. Coverage includes concurrent initial publications, one-comment updates,
late older reports/failures, non-default targets, invalid envelopes, closed/stale
PRs, and default-branch baseline publication. The companion branch is
[`feat/generated-comparison-comments`](https://github.com/pawelchcki/my-infra/tree/feat/generated-comparison-comments),
with the final ordering/serialization fix at `4759fad`. It has not been deployed.

The initial live BuildBuddy regression submission targeted `workflows`, which
had no registered Linux amd64 executor. A subsequent host inspection found two
active executors, `ccd0` and `ccd1`, configured for **`linux-amd64-kvm`**. Both run
on Bazzite and share one 24 GiB `gfx1100` Radeon; the host also has a 512 MiB
integrated GPU. Executor defaults enable bare-host execution and Podman.

A [live hardware probe](https://pawel.buildbuddy.io/invocation/8259dbcb-8294-4c43-9009-fd4aa46f280c)
completed successfully in `linux-amd64-kvm` with `workload-isolation-type=none`
and `container-image=none`. Its retained JSON artifact confirms host `bazzite`,
read/write access to `/dev/kfd` and both render nodes, and the discrete GPU at
PCI `0000:03:00.0` with 25,753,026,560 bytes of VRAM. This establishes scheduling
and device access, not model inference or device access inside an immutable
container image. Scoring jobs must serialize access to the shared discrete GPU
across both executors.

The comparison workflow remains manual while generated-comment publication awaits
the companion ci-toolkit release. The Bazzite worker setup now uses a separately
fingerprinted ROCm profile, described below. Existing Linux GitHub Actions consumer
coverage remains until the corresponding BuildBuddy CPU workflow passes.

On 2026-09-12, both host executors received the GPU labels documented in
[README.md](README.md#bazzite-executor-labels). Each was confirmed idle, then
restarted sequentially; both registered and transitioned to ready. A
[post-change hardware probe](https://pawel.buildbuddy.io/invocation/596ebaa0-8410-4687-9275-483a5ca2465e)
completed successfully on Bazzite and retained the same GPU-access evidence.
The pool and isolation settings were preserved. GPU labels do not implement
shared-device locking.

## Live Bazzite dogfooding, 2026-09-12

`ccd1` now registers in the dedicated **`linux-amd64-rocm`** pool; `ccd0` retains
`linux-amd64-kvm`. This followed a real coordinator being routed to
`tvrepublika-wsl` in the general pool despite a Bazzite label preference. The
failed invocation remains retained. The dedicated executor was idle before its
restart, registered successfully, and became ready. It has 12 logical CPUs and
60 GiB of advertised host memory; the CPU parent and GPU child each request one
compute unit. They can run on the same executor.

Built clean source commit `4646123b274005c587dfeb614f17ddf5fd36aef6` with explicit
source provenance and installed the ROCm scorer under
`/var/lib/llm-cc/scorers/4646123-rocm`. The checked profile uses context **32768**,
batch **256**, Flash Attention on, **Q8_0** K/V, device entropy reduction and the
**64 KiB** file limit. It hashes the installed executable/backend, the existing
DeepSeek Q6_K model, and all eight system libraries used by the binary/backend;
ROCm and its remaining runtime dependencies are inside the checksummed bundle.
The comparison package is a checksummed host ZIP. No model download was needed.

The [largest-file smoke parent](https://pawel.buildbuddy.io/invocation/ecc2a0ce-058a-414f-b5cd-595a6f71ac38)
and [GPU child](https://pawel.buildbuddy.io/invocation/8d4fa7ef-6ad4-42cb-88af-d7a670fcef69)
both completed successfully. The parent submitted exactly one GPU worker, which
validated full JSONL results for committed `src/score_cmd.cc` (62,346 bytes;
19,250 tokens) and `src/cli/main.cc` (56,147 bytes; 16,942 tokens). The worker
completed in 71.0 seconds; the parent comparison stages took 82.75 seconds.
The parent retained the plan, worker result, reports, publication envelope and
acceptance summary. These are fixture acceptance results, not repository totals.

All **59** comparison tests pass directly and through Bazel. New checks cover
ROCm profile fingerprints and manifest integrity; real-device eligibility;
runtime mismatch; shared-lock contention and inherited descriptor lifetime;
process-group cleanup on the wall-clock deadline; checksum-verified code bundles;
atomic host configuration; and actual BuildBuddy parent metadata resolution.
The bundle test uses an explicit ZIP timestamp for Bazel's test environment.

The [production baseline parent](https://pawel.buildbuddy.io/invocation/34802c6c-82ae-48ef-9065-f7afa02d8659)
resolved the actual default-branch head, reused the two smoke results, and sent
107 misses across eight languages to
[one GPU child](https://pawel.buildbuddy.io/invocation/55868cdf-d252-4ba2-8b50-8acd57a61b2a).
Both passed. Parent duration was **297.48 seconds**, worker **281.89 seconds**.
All 109 eligible paths were measured: **236,259 tokens**, raw LLM-CC total
**24,718.8**, repository score **0.1046258555**, supported-file coverage **100%**.
The 18 excluded and 240 unsupported paths remain visible. During this baseline
worker, host sysfs sampling observed a maximum of **16,974,262,272 bytes
(15.81 GiB)** of VRAM in use; the earlier largest-file smoke was outside that
sampling interval.

The [production warm parent](https://pawel.buildbuddy.io/invocation/ce922b00-77a6-4443-9905-58cc35e0d0bb)
completed in **3.20 seconds**, including the BuildBuddy parent execution. Its
plan has **109 hits, zero misses, zero workers**, and no submission artifact.
Every result and aggregate total exactly matches the baseline report. The
report identity matches that actual parent invocation. This used the shared
host filesystem; S3/network-store latency was not measured.

The [incremental fixture parent](https://pawel.buildbuddy.io/invocation/41d462a0-4dd9-4b09-ba71-4dfc12675c46)
and [child](https://pawel.buildbuddy.io/invocation/cdb70567-6f60-4e6f-b94f-137bceec43e8)
passed with **two hits, one miss, one GPU submission**, and correctly reported
**one addition and one rename**. Comparison stages took **42.65 seconds**. It
was submitted while the baseline was active and uses the same host GPU lock.

The renamed external consumer's CPU `--tests-only` check passed in 240 seconds:
all five targets, all four stage launchers, and backend-ops label analysis.
Portability-only test adjustments passed separately and in the final root suite.

Host assets and manual submission are active through
`/var/lib/llm-cc/comparison.json`; see [BAZZITE.md](BAZZITE.md). Automatic workflow
triggers and generated PR comments remain disabled pending the companion
publisher's normal release and PR publication acceptance.

Before opening the PR, independent review added cache-corruption, Git submodule,
case-sensitive path classification, malformed-worker-artifact and rendered-path
regressions. The final **64-test** comparison suite and installer/CLI/language/TLS
targets pass through Bazel. Repository formatting and Ruff checks also pass.
