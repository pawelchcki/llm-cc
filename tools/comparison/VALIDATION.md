# Implementation validation

The sections up to 2026-09-23 record the Python comparison stages that
`llm-cc compare` has since replaced; their measurements describe that
implementation. See [the comparison core in llm-cc](#comparison-core-in-llm-cc-2026-09-25)
for the current one.

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
- Warm-preparation cache concurrency, measured on 2026-09-15 on this repository
  with 132 seeded results, zero misses and an empty worker plan. `plan.json` was
  byte-identical across all four runs.

  | Store | `--cache-concurrency 1` | `--cache-concurrency 8` |
  |---|---:|---:|
  | Local filesystem | 0.053 s | 0.056 s |
  | 20 ms injected read latency | 2.707 s | 0.385 s |

  A local filesystem cache is already fast enough that the bound does not
  matter; the default of 8 exists for high-latency object stores. The Python
  script that produced this table was removed with the Python stages;
  `llm-cc compare prepare --cache-concurrency N` takes the same bound.

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

At this point the comparison workflow was still manual, pending the companion
ci-toolkit release; see [automatic publication](#automatic-publication-2026-09-12).
The Bazzite worker setup now uses a separately fingerprinted ROCm profile,
described below. Existing Linux GitHub Actions consumer
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
triggers and generated PR comments were still disabled at this point, pending
the companion publisher's release; they were enabled later that day.

Before opening the PR, independent review added cache-corruption, Git submodule,
case-sensitive path classification, malformed-worker-artifact and rendered-path
regressions. The final **64-test** comparison suite and installer/CLI/language/TLS
targets pass through Bazel. Repository formatting and Ruff checks also pass.

## Real PR comparison acceptance, 2026-09-12

Manual production comparisons exercised [PR #39](https://github.com/pawelchcki/llm-cc/pull/39)
at head `574f2c330158d8844d1a96cc47bbed809fc572e8`, targeting actual `main` SHA
`4646123b274005c587dfeb614f17ddf5fd36aef6`. The resolved merge base is that same
target commit. These measurements apply to this recorded head; later PR updates
require another comparison. The installed comparison ZIP was pinned by SHA-256
`83ebbf2ea24ddfe1d918c3a018bd40c309bd34c23274c72622567348e19ffa6a`.

The [cold PR parent](https://pawel.buildbuddy.io/invocation/0dbc59cd-813f-4b82-a717-b60c30bc6e91)
passed on Bazzite in **119.627 seconds**. It reused **109** baseline cache entries
and submitted **25** unique misses to
[one GPU worker](https://pawel.buildbuddy.io/invocation/6d407e58-42af-4cff-9061-d78c7c17e78c),
which completed in **105.053 seconds**. Supported-file coverage was **100%** on
both revisions: **109/109** base paths and **130/130** head paths measured.

| Repository metric | Base | PR head |
|---|---:|---:|
| Raw LLM-CC total | 24,718.8 | 32,368.4 |
| Tokens | 236,259 | 296,833 |
| Score | 0.1046258555 | 0.1090458271 |

The repository score increased **4.22455%**; the invocation still passed, as
required for advisory reporting. Runtime score was unchanged. Tests and tooling
scores increased **6.42962%** and **9.90908%**, respectively.

The [warm PR parent](https://pawel.buildbuddy.io/invocation/d53eddc4-98b3-40fe-a1f8-2852262a259b)
passed in **4.304 seconds**, with **134 cache hits, zero misses and an empty GPU
worker plan**. Its artifacts contain no worker submissions or worker result file.
Every per-file result and category/repository comparison exactly matches the
cold report. Parent durations above use BuildBuddy's invocation creation/update
timestamps; worker duration comes from its validated result artifact.

Both publication envelopes match PR #39, the recorded head and target, and their
actual parent invocation IDs. Comment byte counts and SHA-256 checksums validate.
Local credentials were used only for normal BuildBuddy API authentication;
requests forwarded no local credentials into jobs. BuildBuddy supplied its own
runner credential, and public GitHub PR discovery required no local GitHub token.

These runs validate manual report generation and retention, including the real
PR-target lookup and fully cached CPU path. They do **not** establish publication
of a generated PR comment. Automatic comparison triggers were still disabled
when they ran.

## Automatic publication, 2026-09-12

[#40](https://github.com/pawelchcki/llm-cc/pull/40) enabled the `Complexity
comparison` triggers in `buildbuddy.yaml` for `main` pushes and pull requests
targeting `main`, and [#41](https://github.com/pawelchcki/llm-cc/pull/41) added
the ci-toolkit publication policy to `.ci-toolkit.yml` on `main`. On PR #40 head
`95a5ea550935`, the update automatically started
[comparison `79c97a2a`](https://pawel.buildbuddy.io/invocation/79c97a2a-5db2-45cb-80b0-268a5bd16c02),
which completed with 130 cache hits, zero GPU workers and 100% supported-file
coverage, and ci-toolkit advanced
[its comment](https://github.com/pawelchcki/llm-cc/pull/40#issuecomment-5647344287)
from pending to the complete table. Delayed or failed publication and
retargeted or closed pull requests have not been exercised live in this flow.

## CI recipe, 2026-09-23

Checked on Linux x86_64 for [CI_RECIPE.md](CI_RECIPE.md). These results
establish local correctness only.

- All **193** comparison tests pass directly and through
  `bazel test //tools/comparison:comparison_test`. The 54 new tests cover GitHub
  discovery and comment pagination (17), publication ordering, suppression and
  validation (17), the CI adapters (10), conditional store writes and concurrent
  native publication (5), and the offline recipe with template invariants (5).
- The offline recipe run (`test_recipe`) executes every generated GitLab child
  job in a real shell, in its own directory with only its declared artifacts,
  against a local repository, an in-memory GitHub and a filesystem store. It
  seeds the default-branch baseline, skips a branch without a pull request,
  scores exactly two new contents on two workers for the cold PR run, and
  handles a rename and a duplicate on the second push with an aggregation-only
  child that updates the same comment. It then rejects the delayed older
  pipeline, reports an oversized file as unmeasured, and suppresses publication
  after a retarget. The warm path, from discovery through publication including
  the aggregation job's subprocesses, took **0.173 to 0.181 seconds** over five
  runs, against the issue's 60-second budget. Object-store latency is not
  included; see the cache concurrency table above.
- The renamed consumer's `BACKEND=cpu tools/check_consumer.sh --tests-only`
  passed all five targets, and all eight stage launchers (`prepare`, `worker`,
  `aggregate`, `compare`, `discover`, `store-report`, `publish`, `ci`) resolved
  their runfiles.
- `tools/check_format.sh` and Ruff's pyflakes rules pass. The GitLab template
  parses as YAML; ShellCheck is clean on each of its job scripts and on
  `build.sh`; actionlint reports only the workflow's custom self-hosted `gpu`
  runner label.
- The coordinator image built with podman from a context assembled as
  `build.sh` does, with a synthetic profile. It runs as uid 10001 with Python
  3.12.14, Git 2.47.3 and boto3 1.42.84, and contains no model files. The
  recipe, publisher, GitHub, cache, pipeline and BuildBuddy tests pass inside
  it, and it reads a checkout owned by another uid. One existing worker test
  fails in that image only because its inline synthetic scorer starts with
  `#!/usr/bin/env python3`, which the worker's sanitized `PATH` cannot resolve
  in `python:3.12-slim`; the shared fixture scorer now names its interpreter
  exactly.

Not verified here: building the model and scorer images (the 14 GB download and
the CUDA build), model-layer reuse across scorer builds, GPU scoring inside the
scorer image, S3 conditional writes against a real service, and a live GitLab or
GitHub Actions rollout.

## Comparison core in llm-cc, 2026-09-25

Selection, classification, planning, scoring, caching and reporting moved into
`llm-cc compare` (schema version 2); this package keeps discovery, CI job
generation, publication and the BuildBuddy/Bazzite coordinator. Checked on
Linux x86_64. These results establish local correctness only.

- `bazel test //tools/comparison:comparison_test` passes all **106** Python
  tests in four shards. The end-to-end tests run `llm-cc-compare-fake`, llm-cc
  with a deterministic scorer, as every stage: the recipe test prepares with
  `llm-cc compare prepare @scoring.args`, runs each generated GitLab worker job
  (`llm-cc compare worker`) in a real shell and aggregates; the BuildBuddy tests
  prepare real v2 plans, upload their blobs verified against Git object IDs, and
  run the submitted remote worker through a stand-in for a verified bare host.
  Plans from another build, changed plan digests and tampered blobs fail with a
  stored v2 worker artifact.
- Bazzite setup derives `scoring.args`, `execution-host.json` and
  `identity.json` from `llm-cc compare identity`; the consumer and example
  rules load through llm-cc's own rules engine.

Not verified here: the image builds, a live GitLab, GitHub Actions or
BuildBuddy run with the new stages, and a Bazzite redeploy.
