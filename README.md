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
`bazel build //:llm-cc` selects CPU on Linux and Metal on macOS. The Linux CPU
binary includes no CUDA or ROCm implementation, but retains the dynamic backend
loader and exported ggml ABI so fetched GPU bundles can be selected later. On
Linux x86-64, `--config=universal` opts into the fat CPU + CUDA + ROCm build.
Its private GPU modules stay in Bazel runfiles during development; packaging
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
On macOS, both the default build and `--config=metal` produce the standalone
static Metal Mach-O executable.

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
--batch-size N
--entropy-reduction auto|host|device
--hierarchy structural|reference
--score lmcc|density|mean
--tau N
--tau-percentile N
--hotspots N
--format jsonl|text
--progress auto|always|never
--alpha N
```

Automatic detection recognizes `.rs`; `.c`; C/C++ headers and `.cc`, `.cpp`,
`.cxx`, `.c++`; CUDA `.cu` sources and `.cuh` headers through the C++ grammar;
`.java`; `.py`, `.pyw`, `.pyi`; `.go`; `.js`, `.mjs`, `.cjs`; and `.cs`,
`.csx`. In addition to the canonical `--lang` names above, `c++`,
`py`, `golang`, `js`, `node`, `nodejs`, `node.js`, `cs`, and `c#` are accepted
as aliases. JavaScript support is for Node.js runtime files only; JSX and all
TypeScript variants are intentionally excluded.

The default headline score is `lmcc_per_token`, selected with `--score lmcc`.
This is a per-token heuristic: it divides raw LM-CC by the number of scored
tokens, but is neither length-invariant nor independently validated. The raw
`llm_cc` field remains available and is length-dependent. `--score density`
selects the fraction of tokens at or above tau, while `--score mean` selects
mean token entropy. The other metrics remain present regardless of the
headline mode.

`--hierarchy structural` is the default. It combines entropy boundaries with
AST scope terminations, uses the first meaningful source byte for scope
membership, and excludes whitespace-only intervals. `--hierarchy reference`
uses the entropy-led, logical-nonblank-line partitioning contract from the
pinned authors' revision `c38a26afdfc29ee517d734c6b677a4d6c65ec59b` for
Python. Other languages apply the same policy with their tree-sitter scopes as
language extensions. Both file and function analysis use the selected mode,
and functions continue to use their file's resolved tau.

Comments and actual Python module, class, function, nested-function, and async
function docstrings are removed before inference. Newlines and mappings to the
original source are preserved. Assigned strings, non-docstring string
expressions, bytes literals, and f-strings remain source content.

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
2. `configuration`: resolved language/discovery options, model, context, batch
   size, hierarchy and entropy-reduction modes, score mode, tau rule, hotspot
   count, alpha, backend, inference ABI, and cache identity.
3. Zero or more `warning` events.
4. A `file_start`, then either `file` or `error`, for each source.
5. `totals` with additive project metrics, per-language totals, and `partial`.

Consumers must wait for exit status zero and a terminal `totals` event with
`partial == false`, then aggregate every `file` event. A `file_start`, a
truncated stream, or an early file with no matching functions is not a complete
project result. `--progress always` writes throttled model, file, cache, and
token progress to stderr while leaving stdout JSONL unchanged; `auto` enables
it only when stderr is a terminal.

A `file` event retains `llm_cc`, `total_branch`, `total_comp_level`, `alpha`,
`tau`, and the complete `units` hierarchy. It also contains normalized metrics,
the selected headline `score`, per-function scores, entropy hotspots, the
canonical `path`, resolved `language`, and `entropy_cache_hit`. Configuration,
file, and totals events contain `analysis_version` and `hierarchy_mode`. An individual
file failure does not stop later files. Exit status is 0 for complete success,
1 for partial results, and 2 for configuration or model failures.

The default maximum input context is 131,072 tokens and the default batch size
is 64. One inference context is reused across files, grows geometrically as
needed, and has its model memory and positions cleared between inputs. Use
`--context` and `--batch-size` to tune these limits. Inference uses the CPU by
default; request GPU offload with `--gpu-layers`.

`--entropy-reduction auto` performs entropy and observed-token log-probability
reduction on the GPU when full offload (`--gpu-layers -1`) guarantees that the
final logits execute there; partial offload conservatively reports a host
fallback because output-layer placement is model-dependent. `host` retains the
ordinary full-logits path. Explicit `device` likewise requires full offload.
The device graph uses a
max-shifted reduction and transfers two floats per scored row instead of a
vocabulary-sized logits row. Host/device comparisons allow `1e-5` nats for
entropy and `1e-5` absolute plus relative tolerance for log probability;
classifications within that distance of tau can be floating-point sensitive.

GPU-backed invocations are serialized per user and backend, so concurrent
`llm-cc` processes wait instead of competing to load the same large model.
Routine llama.cpp initialization diagnostics are suppressed; actionable
warnings and errors still go to stderr. If a Codex sandbox reports zero GPU
memory, `llm-cc` identifies the sandbox and recommends running with permission
to access the accelerator directly.

## Models

Without `--model`, `llm-cc` uses the first entry in its built-in registry,
`deepseek-coder-v2-lite-base-q6_k`. The registry also contains
`deepseek-coder-6.7b-base-q6_k`, `qwen2.5-coder-1.5b-q6_k`,
`qwen2.5-coder-3b-q6_k`, and
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

Two smaller alternatives are available:

| Model name | Approximate download | Guidance |
| --- | ---: | --- |
| `qwen2.5-coder-1.5b-q6_k` | 1.3 GB | Lower memory use; rank correlation 0.90 with the default in our sample. |
| `qwen2.5-coder-3b-q6_k` | 2.5 GB | Closer rankings; correlation 0.93 and the same top five files in our sample. |

```sh
llm-cc src --model-name qwen2.5-coder-1.5b-q6_k --context 32768
llm-cc src --model-name qwen2.5-coder-3b-q6_k --context 32768
```

Both have a native 32,768-token context. Changing models requires a fresh
score baseline. The [model-selection experiment](experiments/model-selection/README.md)
contains reproducible timings, memory measurements, and scoring comparisons
on pinned `dd-trace-c` history; it measures agreement with the default, not
downstream coding quality.

## Raw scoring

The scoring contract remains available as a subcommand:

```sh
llm-cc score --model model.gguf --entropy --prompt 'The quick brown fox'
llm-cc score --model model.gguf --entropy --file source.cpp
```

`score` accepts a local `--model` or a registered `--model-name`, plus
`--prompt` or `--file`, `--bos auto|always|never`, `--context-size`, `--threads`,
`--gpu-layers`, `--backend`, `--backend-dir`, `--batch-size`,
`--entropy-reduction`, `--no-download`, `--override-memory-check`, and
`--entropy`. It emits one JSONL object per observed token. It never samples or
generates a continuation. Its default maximum input context is 131,072 tokens;
use `--context-size` to override it. Mean negative log-likelihood and perplexity
go to stderr.

```json
{"position":0,"token_id":785,"piece":"The","bytes_hex":"546865","probability":null,"log_probability":null,"entropy":null}
```

The analyzer calls the same scorer in-process and consumes typed records; no
JSONL parse or subprocess is involved. One model is loaded per analyzer
invocation, on the first entropy cache miss. Misses share a bounded reusable
inference context; an all-hit invocation never loads model weights.

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
manifest from the same directory. Cache hits revalidate both the bundle footer
and, for stamped binaries, the manifest's build commit. `remove` deletes final
files and any resumable `.partial` downloads. Automatic GPU resolution uses
this order:

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
on a tie. CUDA driver availability is checked before resolving or fetching its
bundle. Explicit `cuda` or `rocm` loads only that GPU plugin plus CPU and fails
clearly when the requested device is unavailable. `cpu` rejects nonzero GPU
layers.

An embedded universal ELF reads its raw payload footer through `/proc/self/exe`.
CUDA is copied into an immutable sealed memfd and never touches disk. ROCm's
exact pinned userspace and architecture-data closure is atomically materialized
under a content-addressed, owner-only cache. CPU execution does not inspect or
materialize either GPU payload.

## Shared entropy cache

Token bytes and entropy values are stored as versioned CBOR in one private,
user-scoped cache that is shared by repositories, linked worktrees, and files
outside Git. The base directory follows this precedence:

```text
$LLM_CC_ENTROPY_CACHE_DIR
$XDG_CACHE_HOME/llm-cc/entropy
$HOME/.cache/llm-cc/entropy
```

Entries live in the `v2/entropy/` namespace. Analysis never modifies
`.gitignore`, `.git/info/exclude`, Git configuration, or the source tree.

Cache keys cover the SHA-256 of prepared source and model contents, inference
ABI, requested runtime backend and GPU-layer policy, context limit, batch size,
requested reduction policy, and effective reducer. Tau, alpha, hierarchy mode, and score mode are
deliberately excluded, so changing them recomputes the inexpensive hierarchy
from cached entropy. Model digests are memoized separately using file identity,
size, and high-resolution modification/change timestamps. Unchanged weights
avoid another full read; moving or copying identical weights retains the same
entropy key after verification. `--no-cache` skips model hashing.

Reads validate that token bytes cover the complete preprocessed source.
Corrupt entries become misses, writes use same-directory atomic replacement,
and owned cache-directory symlinks are never followed. Hits refresh recency
after checking expiry. Housekeeping runs at most daily during use and on explicit
pruning. Before publication, expired entries are removed and the least recently
used entries are evicted as needed to keep committed entropy files within 1 GiB.
Entries expire after 20 days without use; oversized entries and entries that
cannot fit after eviction are not cached. Filesystem allocation overhead,
temporary writes, and small metadata files are additional. Completed model
weights, resumable downloads, digest memos, and backend bundles remain separate.

Inspect or prune the shared cache. An optional `PATH` adds a report for the
older repository-scoped v1 stores only:

```sh
llm-cc cache status [PATH] [--format text|json]
llm-cc cache prune  [PATH] [--format text|json]
llm-cc cache clear --all [--format text|json]
llm-cc cache clear  [PATH] [--format text|json]
llm-cc cache clear  [PATH] --legacy [--format text|json]
```

Status reports the user scope, v2 storage version, limit, retention, entry
count, bytes, and malformed entries. The former v1 stores are cold and are
never promoted because they do not contain content-based model provenance.
`cache clear --all` clears the shared v2 cache. A path-scoped `clear` removes
only its old v1 bucket; `--legacy` additionally clears its former local
`.llm-cc-cache/llm-cc/v1/entropy/` namespace.
Without `PATH`, the scoped clear uses the current repository.

## Hermetic Linux build

Linux x86-64 builds use checksum-pinned Clang 22 and a Debian Stretch glibc
2.24 sysroot by default. The shipped ABI contract remains glibc 2.28 or newer;
CI rejects any imported symbol above that ceiling. The `portable` profile is
retained as an explicit alias:

```sh
bazel build --config=release --config=portable --config=universal //dist:linux_x86_64
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
