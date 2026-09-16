# llm-cc

For advisory base/head complexity reports with reusable filesystem or S3 results
and CPU-only cache hits, see [the comparison stage guide](tools/comparison/README.md).

`llm-cc` measures entropy-guided language-model code complexity (LM-CC) in Rust,
C, C++, Java, Python, Go, Node.js JavaScript, and C#. It helps identify files,
functions, and source lines to inspect when planning refactoring, using a local
GGUF model through llama.cpp.

## Motivation

Code quality is an investment in readability, maintainability, and reliable
work by both people and coding agents. Clear structure makes changes easier
to understand and review. LM-CC adds a model-sensitive refactoring signal
alongside tests, code review, and engineering judgment; a lower score alone
cannot establish that a change improves the code.

The paper *Rethinking Code Complexity Through the Lens of Large Language
Models* describes [semantic decomposition](https://arxiv.org/html/2602.07882v1#S3.SS2)
using token uncertainty and structural boundaries, then defines
[LM-CC](https://arxiv.org/html/2602.07882v1#S3.SS4) from branching and
compositional depth. The `llm_cc` field, and the default headline score, is that
LM-CC: `0.8 × branches + 0.2 × Σ depth` over the block tree, on the same scale
as the paper's reported values (tens for a typical program). This
implementation removes comments and Python docstrings before measuring entropy
and constructing that hierarchy.

The paper's [rewriting experiment](https://arxiv.org/html/2602.07882v1#S4.SS2)
reports program-repair pass@1 increasing from **13.4% to 16.2%** on its selected
rewrite subset. Selection required lower LM-CC without lower cyclomatic
complexity; passing original tests was also required outside the repair task.
That result motivates investigating refactoring opportunities, but does not
validate every rewrite, this implementation's deviations from the reference, or
its optional normalized scores.

### Relation to the reference implementation

The per-token signal and the score formula match the authors' reference
implementation ([xchen121/lm-cc](https://github.com/xchen121/lm-cc) at
`c38a26af`): full-vocabulary entropy in nats at temperature 1, logits at
position *i − 1* scoring token *i*, and a first code token that never opens a
block. The paper sets τ = 0.67 nats from a percentile of CodeLlama-7b's token
entropies. Entropy distributions differ between models, so an absolute 0.67
marks a different share of tokens for each one. Every registered model
therefore carries its own τ: the entropy percentile at which the authors'
CodeLlama-7b pipeline reaches exactly 0.67 on the paper's HumanEval programs,
applied to that model's entropies on the same programs. See the
[τ calibration experiment](experiments/tau-calibration/README.md). A custom
`--model` falls back to 0.67; `--tau-percentile` applies a percentile per file
instead.

These deviations are deliberate:

- The default `--hierarchy structural` adds syntax scope boundaries to the
  entropy boundaries. `--hierarchy reference` uses entropy boundaries only.
- Nesting comes from tree-sitter scopes in every supported language rather
  than from Python indentation.
- Comments and docstrings are removed with tree-sitter, blank lines are kept,
  and code is not reformatted with `black`.
- The default model is DeepSeek-Coder-V2-Lite rather than CodeLlama-7b. The
  registered `codellama-7b-q8_0` model reproduces the paper's model choice.

## Installation

### From source

Install Git, then clone the repository and select the source revision:

```sh
git clone https://github.com/pawelchcki/llm-cc.git
cd llm-cc
git checkout main
```

Replace `main` with a release tag or commit if you need a particular revision.
Install [Bazelisk](https://github.com/bazelbuild/bazelisk#installation), which
automatically downloads and runs the Bazel version pinned in [.bazelversion](.bazelversion).
On macOS with Homebrew:

```sh
brew install bazelisk
```

On Linux x86-64, download the Bazelisk binary and install it as `bazel`:

```sh
mkdir -p "$HOME/.local/bin"
curl -fL https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64 \
  -o "$HOME/.local/bin/bazel"
chmod +x "$HOME/.local/bin/bazel"
```

For Linux ARM64 CPU builds, use the `bazelisk-linux-arm64` asset instead. Add
the installation directory to `PATH` on either platform, and keep this setting
in your shell startup file:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

Run the command matching your platform from the checkout:

| Platform | Prerequisites | Installation command |
| --- | --- | --- |
| macOS Metal | macOS 14+, Metal-capable hardware, full Xcode selected through `xcode-select` | `bazel run --config=release --config=metal //:install` |
| Linux AMD ROCm | x86-64, a supported `gfx1100`/`gfx1101`/`gfx1102` GPU, compatible AMD driver, GPU device permissions | `bazel run --config=release --config=rocm //:install` |
| Linux NVIDIA CUDA | x86-64, supported NVIDIA GPU, driver compatible with CUDA 13.0.2 | `bazel run --config=release --config=cuda //:install` |

Bazel downloads the pinned Linux compilers and GPU SDKs; a system CUDA or ROCm
SDK is unnecessary. Drivers and device access remain host prerequisites. Follow
[AMD's device-access guidance](https://rocm.docs.amd.com/projects/install-on-linux/en/docs-7.1.1/install/prerequisites.html#configuring-permissions-for-gpu-access)
and [NVIDIA's driver compatibility guidance](https://docs.nvidia.com/deploy/cuda-compatibility/minor-version-compatibility.html)
for your system. macOS builds use the selected Xcode and Apple SDK.

For a clean Ubuntu build container, install `ca-certificates`, `libxml2`,
`libstdc++6`, `zlib1g`, `python3`, `git`, `perl`, `xz-utils`, `bzip2`, and `unzip`,
plus Bazel/Bazelisk. The pinned LLVM linker needs `libxml2.so.2` at build time.
GNU Make is unnecessary: Linux TLS and payload hashes use native Bazel BoringSSL
`0.20260813.0`, and curl uses pinned CMake/Ninja. macOS uses Secure Transport;
Windows uses Schannel. BoringSSL updates must pass the local TLS fixture because
upstream does not promise API/ABI stability.

To consume llm-cc from another Bazel module, use the complete
[consumer example](examples/consumer/README.md), including root-owned patches,
toolchain registration, toolkit overrides, and explicit llm-cc provenance.
For A10-only builds, add `--//:cuda_archs=compute_86:sm_86` in this checkout or
`--@llm_cc//:cuda_archs=compute_86:sm_86` in the consumer. The default preserves
portable release architecture coverage. Bundles and caches carry a configuration
fingerprint, so narrower builds cannot reuse incompatible portable bundles.

For a CPU installation:

```sh
bazel run --config=release --config=cpu //:install
```

The default prefix is `$HOME/.local`, placing the executable in its `bin`
directory. Append `-- --prefix PATH` to any install command to change it, for
example:

```sh
bazel run --config=release --config=metal //:install -- --prefix "$HOME/opt/llm-cc"
```

Add that prefix's `bin` directory to `PATH` instead. Verify the installation:

```sh
llm-cc --version
```

### Prebuilt releases

Download an executable from [GitHub Releases](https://github.com/pawelchcki/llm-cc/releases),
or use the included installer with Python 3.9+ on macOS or Linux:

```sh
python3 tools/install_release.py
```

On Windows, use the Python launcher:

```powershell
py -3 tools/install_release.py
```

The installer selects your platform, verifies SHA-256, and installs into
`$HOME/.local/bin` (or `%USERPROFILE%\.local\bin` on Windows). Ensure that
directory is on `PATH` before running `llm-cc`; pass `--bin-dir PATH` to choose
another location. Linux x86-64 releases require glibc 2.28 or newer and can
download matching CUDA/ROCm bundles. Linux ARM64 releases require glibc 2.35 or
newer and use CPU. macOS includes Metal, while Windows x64 uses CPU. Model
weights are separate.

## Usage

Get a readable project report with file scores, function scores, and entropy
hotspots:

```sh
llm-cc src --format text
```

Inputs may be files or recursively searched directories. Languages are detected
per file. Discovery respects Git ignore rules and skips common generated
folders; add `--include-headers` to discover headers. Explicit headers are always
accepted. JavaScript support excludes JSX and TypeScript.

Select your own llama.cpp-compatible GGUF:

```sh
llm-cc src include/widget.hpp --include-headers --model /path/to/model.gguf --format text
```

Without `--model`, the default is `deepseek-coder-v2-lite-base-q6_k` (about
14 GB). Use `llm-cc models list --available` to inspect registered models and
`--model-name NAME` to choose one. `--model` and `--model-name` are mutually
exclusive; model selection never changes automatically.

Missing models require download confirmation. `--assume-yes` (`-y`) accepts
model and permitted backend downloads for unattended use; without it, missing
assets cause a prompt or an error if no terminal is available. `--no-download`
prevents these downloads even with `--assume-yes`.

GPU execution defaults to full offload (`--gpu-layers -1`). Linux supports
`--backend cuda` or `--backend rocm`; Metal uses automatic runtime selection.
GPU setup failures stop with a suggested CPU command. Explicitly request CPU
execution, including after a CPU installation:

```sh
llm-cc src --force-cpu --model-name qwen2.5-coder-3b-q6_k --assume-yes --format text
```

Inference memory is configurable with `--flash-attn auto|on|off`,
`--kv-cache-type f16|q8_0|q4_0` (the same type is used for K and V), and
`--kv-offload on|off`. Quantized V requires Flash Attention, so combining a
quantized cache with `--flash-attn off` is rejected. `--backend-diagnostics`
adds bounded backend logs, device allocation information, and operation
placement to stderr. These settings are part of the entropy-cache identity.

The retained A10G measurement for the 14.07 GB DeepSeek Q6_K artifact peaked
at 17.55 GiB of GPU memory for the 26-file, 32K-context experiment. This is a
measurement, not a universal requirement: context length, batch size, K/V
type, backend, and driver all affect the total. The dedicated
[inference validation experiment](experiments/inference-issues/README.md)
records the corresponding Radeon and CUDA measurements. On a 24 GiB Radeon RX
7900 XTX, a 74,496-token DeepSeek context completed with Q4_0 K/V at a measured
20,232 MiB peak, while Q8_0 and F16 did not fit. The independent 26-file score
agreement gates pass for Flash Attention and Q8_0, so the defaults are
`--flash-attn on --kv-cache-type q8_0`. Q4_0 remains opt-in.

CPU inference can be slow. The registered Qwen 3B model is about 2.5 GB;
`qwen2.5-coder-1.5b-q6_k` needs about 1.3 GB of weights. Allow additional memory
for inference. The [model-selection experiment](experiments/model-selection/README.md)
compares timings, memory, and ranking agreement with the default; it does not
measure downstream coding quality.

For unattended JSONL output, the default format:

```sh
llm-cc src --assume-yes > results.jsonl 2> progress.log
```

Progress goes to stderr. Consumers must collect every `file` event and wait for
both exit status **0** and a terminal `totals` event with `partial == false`.
A truncated stream or `file_start` is incomplete. Exit **1** indicates partial
results; **2** indicates configuration or model failure. Individual file errors
do not prevent later files from being analyzed.

Files larger than 1 MiB produce an informational stderr notice. Inputs larger
than 1 GiB (1,073,741,824 bytes) are rejected, including raw scoring input.
Files exceeding the configured context are scored with overlapping windows:
the first window is scored normally, then each subsequent window advances by
half the context size. Overlapping tokens supply context; each source token is
reported once. Syntax scopes and the complexity hierarchy still cover the
complete file. Files that fit retain their existing scoring behavior.

Windowed scores use bounded preceding context, so they can differ from scores
computed with enough context for the entire file. Changing the context changes
the measurement; use consistent settings when comparing revisions. The report
exposes overflow relative to a fixed 128K-token reference independently of the
inference context. Overflow is a separate policy signal and does not change raw
LM-CC or the default normalized score.

Syntax preprocessing has a five-minute cooperative time budget and a maximum
syntax depth of 1,024. Inputs exceeding either budget fail explicitly; no
truncated analysis is reported. This budget covers parsing and preprocessing,
not model loading, tokenization, or inference. Full neural scoring of tens of
megabytes can take much longer, depending on hardware, model, and context.
Long phases keep emitting five-second heartbeats with phase time and stalled
time; decoding also reports token throughput. `--progress never` suppresses
this status output.

The [large-file experiment](experiments/large-files/README.md) records dense
10 MiB fixtures across all supported languages and 100 MiB C++/Python runs,
including elapsed preprocessing time and peak host memory.

## Interpreting scores

The default headline (`--score raw`) is raw LM-CC, the paper's metric. It
combines branching and compositional depth, so it grows with input length.
Project totals report the sum over files and `mean_llm_cc_per_file`.
`--score lmcc` selects `lmcc_per_token`, raw LM-CC divided by the number of
scored tokens. That normalization is a heuristic: it is neither length-invariant
nor validated by the paper. `--score density` selects the fraction of tokens at
or above the entropy threshold; `--score mean` selects mean entropy. Every
metric remains available in JSONL regardless of the headline.

Compare revisions with the same model, quantization, hierarchy, and inference
settings. Establish a fresh baseline when changing them, including after
upgrading from releases that used a fixed τ of 0.67 for every model or the
per-token headline. The default `--hierarchy structural` combines entropy and
syntax boundaries. Without `--tau` or `--tau-percentile`, the threshold is the
selected registered model's calibrated τ; the JSONL `configuration` event
reports the effective `tau` and a `tau_source` of `model-default`,
`paper-default`, or `cli`. Use scores and hotspots to choose code to inspect,
then assess changes through readability, maintainability, tests, and review.

## Development

The project is a C++20 executable built with Bazel. Run the development checks:

```sh
bazel test //:unit
bazel test //:integration
tools/run_clang_tidy.sh
```

Ordinary pull requests use BuildBuddy's CPU regression, GPU backend, and LMCC
comparison checks. Comparisons run automatically on PR updates; `main` pushes
populate the baseline cache and publish a repository-wide ranking of the
worst-scoring files. ci-toolkit publishes the comparison table, the per-file
changed table, and report links in one updated PR comment using the target
branch's publication policy. Other repositories can adopt the same comparison by
copying the templates in
[tools/comparison/consumer](tools/comparison/consumer/README.md).
The full GitHub Actions platform matrix, CPU/CUDA consumer installation checks,
and cross-platform lockfile check run on `main` pushes and same-repository
release-please PRs, such as `release-please--branches--main`. Manual full runs
also require `main` or a release-please branch. Superseded runs are canceled.

The optional `bazel test //:model_smoke_test` downloads a pinned 398 MB model
for a real CPU inference check. See [DESIGN.md](DESIGN.md) for implementation
background and [CHANGELOG.md](CHANGELOG.md) for changes. `llm-cc --help` lists
all analysis options; `llm-cc score --help` documents raw token scoring.
