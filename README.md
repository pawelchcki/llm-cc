# llm-cc

`llm-cc` computes entropy-guided language-model code complexity (LM-CC) for
Rust, C, C++, Java, Python, Go, Node.js JavaScript, and C#. It is one C++20
binary built entirely with Bazel.

The analyzer removes comments with tree-sitter, obtains teacher-forced token
entropy from a llama.cpp-compatible GGUF, detects semantic boundaries, builds
the paper's compositional hierarchy, and streams compact JSONL.

## Build

Bazelisk reads the pinned Bazel version automatically:

```sh
bazel build //:llm-cc
bazel test //:unit
bazel test //:integration
```

Run `bazel test //:model_smoke_test` to opt into a CPU-only check against the
smallest registered real model; this downloads a checksum-pinned 398 MB GGUF.

The build downloads checksum-pinned LLVM, llama.cpp, tree-sitter and its Rust,
C, C++, Java, Python, Go, JavaScript, and C# source bundles, nlohmann/json,
curl, and, on non-macOS platforms, OpenSSL. macOS curl builds use Apple's
Secure Transport and system trust store.
`bazel build //:llm-cc` builds the CPU-only binary by default. On Linux x86-64,
`--config=universal` opts into the fat CPU + CUDA + ROCm build. Its private GPU
modules stay in Bazel runfiles during development; packaging
`//dist:linux_x86_64` appends them to the executable. The universal build is
slow and needs about 80 GB of disk. Other explicit backend builds remain
available:

```sh
bazel build --config=release --config=universal //:llm-cc
bazel build --config=release --config=rocm //:llm-cc
bazel build --config=release --config=cuda //:llm-cc
bazel build --config=release --config=metal //:llm-cc
bazel build --config=release --config=cpu //:llm-cc
```

ROCm builds download AMD's checksum-pinned TheRock 7.14.0 `gfx110X` SDK and
compile deterministic code objects for `gfx1100`, `gfx1101`, and `gfx1102`.
CUDA builds assemble a checksum-pinned CUDA 13.0.2 subset from NVIDIA's
redistributable component archives. NVCC compiles device code one translation
unit per Bazel action and delegates its host phase to pinned Clang 22. CUDA's
x86 headers reject libc++, so that host phase reads pinned libstdc++ headers;
no GCC executable runs. Application, CPU, and HIP code all compile with pinned
Clang and libc++. Linux targets use a checksum-pinned Debian Stretch sysroot,
and neither backend searches the host for a vendor SDK, compiler, C library,
or GNU Make.

HIP sources receive repository-relative deterministic compilation-unit IDs;
HIP/CUDA host paths are prefix-mapped and optional host ccache discovery is
disabled. This keeps sandbox locations out of accelerator artifacts.

Metal builds embed the Metal source in the binary instead of producing a
machine-specific `.metallib`. Bazel uses the Xcode selected by `xcode-select`,
while `--config=metal` retains a macOS 14.0 deployment target. Xcode and the
Apple SDK are licensed host prerequisites and cannot be downloaded hermetically
by the repository.

The ROCm backend targets Linux x86_64 Radeon `gfx110X`. The default CPU build
supports other Linux platforms; change the pinned TheRock artifact and
`GPU_TARGETS` together when adding another AMD GPU family.

Install to `$HOME/.local/bin`, or choose another prefix:

```sh
bazel run //:install
bazel run //:install -- --prefix /opt/llm-cc
```

Installation atomically replaces one `bin/llm-cc` executable. On Linux that
ELF contains the statically linked application and llama/ggml CPU code by
default. With `--config=universal`, it also contains raw CUDA and ROCm payloads.
On macOS, use `--config=metal` for the standalone static Metal Mach-O
executable.

Build deterministic distribution artifacts with:

```sh
bazel build --config=release --config=universal //dist:linux_x86_64
bazel build --config=release --config=metal //dist:macos
```

The Linux target emits `llm-cc-linux-x86_64` and its SHA-256 checksum.
Compatibility labels `//:universal_archive` and `//:install_payload` resolve to
that target. `//:cpu_static_archive` emits
`llm-cc-linux-x86_64-cpu-static.tar.gz`, whose root directory has the same name
without `.tar.gz`. Release automation adds versions to published asset names.

Stamped builds also define `LLM_CC_GIT_SHA` and `LLM_CC_ARTIFACT_BASE_URL` in
the generated version header. Development builds use the configured artifact
resolver (defaulting to `https://ci-artifacts.pawelchcki.workers.dev`), while
exact release tags stamp the matching GitHub release URL. The resolver Worker
is not deployed yet. Until it is, the supported path is
`llm-cc backends fetch <name> --url <public_url>` with the Public URL from the
CI comment. In workspace status output, the literal artifact URL value `none`
means fetching is disabled; it becomes an empty build-time constant.

### Pull-request GPU bundles

Each pull request publishes immutable CUDA and ROCm backend bundles. An
automated pull-request comment links the bundles, their checksums, expiration
dates, and stable resolver URLs. A stamped development build automatically uses
the resolver URL for its commit to fetch the matching backend bundle once the
resolver Worker is deployed.

## Analyze source and projects

Pass any number of files and directories. Directories are searched recursively;
canonical paths are deduplicated and analyzed in sorted order. The language is
inferred per file, or can be forced for every input:

```sh
llm-cc src include/widget.hpp --include-headers --model /path/to/model.gguf
```

Analysis options are:

```text
--lang auto|rust|c|cpp|java|python|go|javascript|csharp
--model GGUF
--model-name NAME
--no-download
--gpu-layers N
--backend auto|cpu|cuda|rocm
--include-headers
--no-ignore
--no-cache
--context N
--score lmcc|density|mean
--tau N
--tau-percentile N
--hotspots N
--format jsonl|text
--alpha N
```

Automatic detection recognizes `.rs`; `.c`; C/C++ headers and `.cc`, `.cpp`,
`.cxx`, `.c++`; `.java`; `.py`, `.pyw`, `.pyi`; `.go`; `.js`, `.mjs`, `.cjs`;
and `.cs`, `.csx`. In addition to the canonical `--lang` names above, `c++`,
`py`, `golang`, `js`, `node`, `nodejs`, `node.js`, `cs`, and `c#` are accepted
as aliases. JavaScript support is for Node.js runtime files only; JSX and all
TypeScript variants are intentionally excluded.

The default headline score is `lmcc_per_token`, selected with `--score lmcc`.
It normalizes LM-CC by the number of scored tokens, making it length-invariant
and suitable for comparing files and functions. The raw `llm_cc` field remains
available, but it is a length-dependent sum. `--score density` selects the
fraction of tokens at or above tau, while `--score mean` selects mean token
entropy. The other metrics remain present regardless of the headline mode.

Tau defaults to an absolute threshold of 0.67 nats. That value was calibrated
by the paper on CodeLlama-7b and may require `--tau` tuning for other models.
Use `--tau-percentile N` to opt into a file-relative percentile instead;
`--tau` and `--tau-percentile` are mutually exclusive. `--hotspots N` controls
the number of highest-entropy source lines reported per file, and 0 disables
hotspots. `--format text` prints a human-readable file/function/hotspot report;
the default `jsonl` format (`json` is an alias) is intended for tooling.

Explicit header files are always accepted. Recursive discovery omits headers
unless `--include-headers` is set. In a Git worktree, discovery uses tracked
files plus unignored untracked files, including nested ignore rules. If Git is
unavailable, the analyzer emits a warning and walks the filesystem. Common
generated and dependency directories are skipped by default. `--no-ignore`
includes Git-ignored and generated sources; `.git/` and `.llm-cc-cache/` remain
excluded.

Analysis output is compact JSONL and each line is flushed immediately. The
event order is:

1. `start`: `discovered` and the requested `model`.
2. `configuration`: resolved language/discovery options, model, context, score
   mode, tau rule, hotspot count, alpha, backend, inference ABI, and cache
   identity.
3. Zero or more `warning` events.
4. A `file_start`, then either `file` or `error`, for each source.
5. `totals` with additive project metrics, per-language totals, and `partial`.

A `file` event retains `llm_cc`, `total_branch`, `total_comp_level`, `alpha`,
`tau`, and the complete `units` hierarchy. It also contains normalized metrics,
the selected headline `score`, per-function scores, entropy hotspots, the
canonical `path`, resolved `language`, and `entropy_cache_hit`. An individual
file failure does not stop later files. Exit status is 0 for complete success,
1 for partial results, and 2 for configuration or model failures.

The default maximum input context is 131,072 tokens. The runtime allocates the
KV cache for the tokenized input rather than eagerly reserving the entire
maximum, so short source files retain a small memory footprint. Use `--context`
to select a different limit. Inference uses the CPU by default; request GPU
offload with `--gpu-layers`.

GPU-backed invocations are serialized per user and backend, so concurrent
`llm-cc` processes wait instead of competing to load the same large model.
Routine llama.cpp initialization diagnostics are suppressed; actionable
warnings and errors still go to stderr. If a Codex sandbox reports zero GPU
memory, `llm-cc` identifies the sandbox and recommends running with permission
to access the accelerator directly.

## Models

Without `--model`, `llm-cc` uses the first entry in its built-in registry,
`deepseek-coder-v2-lite-base-q6_k`. The registry also contains
`deepseek-coder-6.7b-base-q6_k`, `qwen2.5-coder-1.5b-q6_k`, and
`qwen2.5-coder-0.5b-q4_k_m`. Select one with `--model-name NAME`;
`--model-name` and `--model` are mutually exclusive. The tool first checks
`models/<registered file>`, then its model cache. If the model is absent it
downloads it over HTTPS to a `.partial` file and resumes that file on the next
attempt. Interactive downloads show a progress bar, transferred size, and
current average speed. Disable downloading with `--no-download`.

The cache directory follows this precedence:

1. `LLM_CC_CACHE_DIR`
2. `$XDG_CACHE_HOME/llm-cc/models`
3. `$HOME/.cache/llm-cc/models`

Inspect it with:

```sh
llm-cc models path
llm-cc models list
llm-cc models list --available
llm-cc models remove MODEL.gguf
```

`models list` shows cached GGUF files and their timestamps. Add `--available`
to show every built-in model, its approximate download size, and whether its
registered file is cached.

The default model is
[DeepSeek-Coder-V2-Lite-Base Q6_K](https://huggingface.co/bartowski/DeepSeek-Coder-V2-Lite-Base-GGUF).
Base code models are preferable to chat-tuned models because LM-CC measures the
raw next-token uncertainty distribution.

## Raw scoring

The scoring contract remains available as a subcommand:

```sh
llm-cc score --model model.gguf --entropy --prompt 'The quick brown fox'
llm-cc score --model model.gguf --entropy --file source.cpp
```

`score` accepts a local `--model` or a registered `--model-name`, plus
`--prompt` or `--file`, `--bos auto|always|never`, `--context-size`, `--threads`,
`--gpu-layers`, `--backend`, `--no-download`, `--override-memory-check`, and
`--entropy`. It emits one JSONL object per observed token. It never samples or
generates a continuation. Its default maximum input context is 131,072 tokens;
use `--context-size` to override it. Mean negative log-likelihood and perplexity
go to stderr.

```json
{"position":0,"token_id":785,"piece":"The","bytes_hex":"546865","probability":null,"log_probability":null,"entropy":null}
```

The analyzer calls the same scorer in-process; no subprocess or second runtime
is involved. One model is loaded per analyzer invocation, on the first entropy
cache miss. Each missed file receives a bounded inference context; an all-hit
invocation never loads model weights.

## Backends

Manage the versioned CUDA and ROCm bundle cache with:

```sh
llm-cc backends list
llm-cc backends fetch cuda
llm-cc backends fetch rocm --url https://example.invalid/backend.bundle
llm-cc backends path [cuda|rocm]
llm-cc backends remove cuda|rocm
```

`fetch` normally uses the artifact base URL stamped into the binary. `--url`
overrides it with the exact bundle URL, and is the fallback for a build with no
stamped artifact URL; the checksum is fetched from `<url>.sha256` and the
manifest from the same directory. Automatic GPU resolution uses this order:

1. `--backend-dir`, or `LLM_CC_BACKEND_DIR` when the option is absent
2. an embedded payload footer in the executable
3. beside-the-executable and Bazel runfiles plugins
4. the versioned bundle in the runtime cache
5. a fetched bundle, unless `--no-download` is set

Within the configured backend directory, the resolver checks the distribution
bundle name, then `cuda.bundle` or `rocm.bundle`, then the raw shared library.
`LLM_CC_RUNTIME_DIR` overrides the root used for downloaded bundles and
extracted ROCm files. Otherwise the runtime root is
`$XDG_CACHE_HOME/llm-cc/runtime`, or `$HOME/.cache/llm-cc/runtime` when
`XDG_CACHE_HOME` is unset.

`--backend auto` is the default. When GPU layers are requested it totals free
VRAM for each usable family and chooses the larger aggregate, preferring CUDA
on a tie. Explicit `cuda` or `rocm` loads only that GPU plugin plus CPU and
fails clearly when the requested device is unavailable. `cpu` rejects nonzero
GPU layers.

An embedded universal ELF reads its raw payload footer through `/proc/self/exe`.
CUDA is copied into an immutable sealed memfd and never touches disk. ROCm's
exact pinned userspace and architecture-data closure is atomically materialized
under a content-addressed, owner-only cache. CPU execution does not inspect or
materialize either GPU payload.

## Repository entropy cache

Within each containing Git repository, token bytes and entropy values are
stored as versioned CBOR under:

```text
.llm-cc-cache/llm-cc/v1/entropy/
```

Non-Git inputs are analyzed without this cache and produce a warning. Cache
keys cover the SHA-256 of comment-stripped source, canonical model path, model
size and high-resolution modification time, inference ABI, requested runtime
backend and GPU-layer policy, and context limit. Tau, alpha, and score mode are
deliberately excluded, so changing them recomputes the inexpensive hierarchy
from cached entropy.

Reads validate that token bytes cover the complete preprocessed source.
Corrupt entries become misses, writes use same-directory atomic replacement,
and cache-directory symlinks are never followed. Hits update entry timestamps
for LRU accounting. Cleanup runs at most daily: entries unused for seven days
are removed, then the oldest entries are evicted until the repository cache is
at most 512 MiB.

Inspect or clean the repository containing `PATH` (the current directory by
default):

```sh
llm-cc cache status [PATH] [--format text|json]
llm-cc cache prune  [PATH] [--format text|json]
llm-cc cache clear  [PATH] [--format text|json]
```

`clear` removes only the `llm-cc` namespace below `.llm-cc-cache`.

## Hermetic Linux build

Linux x86-64 builds use checksum-pinned Clang 22 and a Debian Stretch glibc
2.24 sysroot by default. The shipped ABI contract remains glibc 2.28 or newer;
CI rejects any imported symbol above that ceiling. The `portable` profile is
retained as an explicit alias:

```sh
bazel build --config=release --config=portable //dist:linux_x86_64
tools/check_glibc_version.sh bazel-bin/dist/llm-cc-linux-x86_64 2.28
tools/check_static_link.sh bazel-bin/dist/llm-cc-linux-x86_64
```

The hermetic Linux build uses static OpenSSL and curl for model downloads. The
CA bundle is selected from `SSL_CERT_FILE` first, followed by common Debian,
Fedora/RHEL, SUSE, and extracted trust-store paths. On macOS, curl uses Secure
Transport and the system trust store by default; `SSL_CERT_FILE` remains an
explicit override.

## Development

```sh
bazel test //:unit
bazel test //:integration
tools/run_clang_tidy.sh
```

The project intentionally uses small standalone C++ tests instead of a test
framework. CI runs the full Bazel test suite and the portable release gates.

## Releases

Conventional Commits drive Release Please, which opens the release pull request
and creates the version tag when that pull request is merged. Each release
includes the CPU binary, standalone CUDA and ROCm backend bundles, and a
`SHA256SUMS` file covering the published assets. Bundle manifests are included
when they are available from the matching commit's GPU build.

Repository: [pawelchcki/llm-cc](https://github.com/pawelchcki/llm-cc)
