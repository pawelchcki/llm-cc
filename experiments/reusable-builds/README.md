Build/install redesign validation, September 10, 2026.

The build benchmark ran on one Linux x86-64 machine with an AMD Ryzen 9 7900
(12 cores / 24 threads), 128 GiB RAM, Bazel 9.2.0, pinned LLVM 22.1.7 and CUDA
13.0.2. Both configurations used optimized release compilation and `--jobs=16`.
Dependencies were downloaded by analysis before measurement. `bazel clean`
removed the action cache before each cold build; disk/remote caches and remote
execution were disabled. Each cold build was followed by an unchanged warm
build. No other compilation or inference checks ran concurrently.

| Configuration | Cold elapsed | Critical path | Sandbox actions | CUDA compile actions | Warm elapsed | Warm compile/link actions |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Portable default (six specifications) | 508.167 s | 189.33 s | 1,216 | 142 | 4.617 s | 0 |
| A10 (`compute_86:sm_86`) | 207.879 s | 53.95 s | 1,216 | 142 | 0.293 s | 0 |

A10 reduced elapsed cold-build time by 59.1%. This is build latency, not model
startup or inference throughput. Both configurations retain identical CUDA
sources and cuBLAS pruning coverage. No kernel selection or scheduling changes
were made. Per-action times reflect concurrent load and are not isolated kernel
compiler benchmarks. Warm timings include Bazel bookkeeping and are too short
to support a meaningful architecture speed comparison.

The longest portable CUDA actions were `fattn-tile-instance-dkq256-dv256.cu`
(180.53 s), `fattn-mma-f16-instance-ncols1_8-ncols2_1.cu` (165.82 s), and
`mmvq.cu` (143.46 s). The full top ten and Bazel action metrics for all four runs
are in [benchmark.json](benchmark.json). Bazel's total action count includes
internal actions: 1,291 for each cold build, and one bookkeeping action for each
warm build. The executed sandbox count above excludes those internal actions.

Reproduce with `tools/benchmark_cuda.sh /absolute/results/directory`. The script
writes JSON profiles, build-event streams, logs and concise summaries. The
reported runs preceded final bundle-verification, whitespace-validation and
declared-input refinements; those do not change the selected CUDA sources or
compiler architecture arguments.

| Issue | Regression evidence |
| --- | --- |
| #26 | Controlled portable/A10 benchmark; architecture analysis and cache/manifest mismatch tests |
| #27 | Complete consumer example; independent Git root with renamed dependency; runfiles tree without `_main`; explicit llm-cc identity |
| #28 | Both CUDA extensions declare OS/architecture dependence; CI transfers a macOS lock to Linux and checks `cublas_headers` and both host entries |
| #31 | Native BoringSSL archives; unused Make sentinel; TLS trust/hostname/redirect/resume fixture; clean Ubuntu build without Make |
| #33 | Restrictive modes/umask, published-directory modes, rollback/concurrency tests; clean-container verification as UID 10001 |

Local CPU checks passed 23 tests, with the embedded GPU payload test skipped in
the CPU configuration. Release-asset Python tests passed. The Linux x86-64 binary
requires at most glibc 2.17 and has no dynamic TLS or C++ runtime dependency.
The `Reusable builds` workflow runs Linux ARM64, macOS and Windows regressions,
plus the external CUDA consumer and macOS-to-Linux lock transfer. Check its PR
results for platform-specific validation rather than inferring support from a
Linux-only local run.

A clean Ubuntu 24.04 container built and installed the CPU configuration without
GNU Make. UID 10001 traversed the installed directories, read installed assets,
executed the binary and ran scoring with a local SmolLM2 135M model and downloads
disabled. The container build has the same glibc 2.17 ceiling.

Clang-tidy reports 295 diagnostics on both this change and the original revision.
A full baseline comparison under the same compiler and headers found no added or
removed diagnostic messages; those existing static-analysis findings remain.
The final A10 installation also passes offline bundle verification and CPU
inference with a local model.

The clean Ubuntu CUDA build/install check also passed without Make. UID 10001
read every installed bundle/manifest/checksum, verified the CUDA bundle offline
without a GPU, and ran local-model CPU inference. The same restrictive-prefix
mode checks passed for CPU and CUDA installations.
