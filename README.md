# llm-cc

`llm-cc` computes entropy-guided language-model code complexity (LM-CC) for
Rust, C, and C++. It is one C++20 binary built entirely with Bazel.

The analyzer removes comments with tree-sitter, obtains teacher-forced token
entropy from a llama.cpp-compatible GGUF, detects semantic boundaries, builds
the paper's compositional hierarchy, and prints JSON.

## Build

Bazelisk reads the pinned Bazel version automatically:

```sh
bazel build //:llm-cc
bazel test //:unit
bazel test //:integration
```

The build downloads checksum-pinned LLVM, llama.cpp, tree-sitter and its Rust,
C, and C++ source bundles, nlohmann/json, curl, and, on non-macOS platforms,
OpenSSL. macOS curl builds use Apple's Secure Transport and system trust store.
On Linux x86-64, `//:llm-cc` is always a universal CPU + CUDA + ROCm
development executable. Its private GPU modules stay in Bazel runfiles, so an
application edit recompiles and relinks only affected application actions.
Metal remains the standalone static macOS default. Smaller diagnostic builds
remain available:

```sh
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

The ROCm archive targets Linux x86_64 Radeon `gfx110X`. Use `--config=cpu` on
other Linux platforms, or change the pinned TheRock artifact and `GPU_TARGETS`
together when adding another AMD GPU family.

Install to `$HOME/.local/bin`, or choose another prefix:

```sh
bazel run //:install
bazel run //:install -- --prefix /opt/llm-cc
```

Installation atomically replaces one `bin/llm-cc` executable. On Linux that
ELF contains the statically linked application, llama/ggml CPU code, and raw
CUDA and ROCm payloads. On macOS it is the standalone static Metal
Mach-O executable.

Build deterministic release archives with:

```sh
bazel build --config=release //dist:linux_x86_64
bazel build --config=release //dist:macos
```

The Linux target emits `llm-cc-0.1-linux-x86_64` and its SHA-256 checksum.
Compatibility labels `//:universal_archive` and `//:install_payload` point to
the same single-file output. `//:cpu_static_archive` remains available as an
explicit CPU diagnostic artifact; it is never the Linux default.

## Analyze source

The language is inferred from the file extension. Use `--lang rust|c|cpp` to
override it:

```sh
llm-cc source.cpp --model /path/to/model.gguf
```

Analysis options are:

```text
--lang rust|c|cpp
--model GGUF
--entropy-jsonl PATH
--no-download
--gpu-layers N
--backend auto|cpu|cuda|rocm
--context N
--tau-percentile N
--alpha N
```

The default maximum input context is 131,072 tokens. The runtime allocates the
KV cache for the tokenized input rather than eagerly reserving the entire
maximum, so short source files retain a small memory footprint. Use `--context`
to select a different limit. Inference uses the CPU by default; request GPU
offload with `--gpu-layers`.

Without `--model`, `llm-cc` first checks
`models/DeepSeek-Coder-V2-Lite-Base-Q6_K.gguf`, then its model cache. If the
default model is absent it downloads it over HTTPS to a `.partial` file and
resumes that file on the next attempt. Interactive downloads show a progress
bar, transferred size, and current average speed. Disable downloading with
`--no-download`.

The cache directory follows this precedence:

1. `LLM_CC_CACHE_DIR`
2. `$XDG_CACHE_HOME/llm-cc/models`
3. `$HOME/.cache/llm-cc/models`

Inspect it with:

```sh
llm-cc models path
llm-cc models list
llm-cc models remove MODEL.gguf
```

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

`score` accepts `--prompt` or `--file`, `--bos auto|always|never`,
`--context-size`, `--threads`, `--gpu-layers`, `--backend`,
`--override-memory-check`, and `--entropy`. It emits one JSONL object per
observed token. It never samples or
generates a continuation. Its default maximum input context is 131,072 tokens;
use `--context-size` to override it. Mean negative log-likelihood and perplexity
go to stderr.

`--backend auto` is the default. When GPU layers are requested it totals free
VRAM for each usable family and chooses the larger aggregate, preferring CUDA
on a tie. Explicit `cuda` or `rocm` loads only that GPU plugin plus CPU and
fails clearly when the requested device is unavailable. `cpu` rejects nonzero
GPU layers. Backend options are invalid with `--entropy-jsonl`, which performs
no inference.

The release ELF reads its raw payload footer through `/proc/self/exe`.
CUDA is copied into an immutable sealed memfd and never touches disk.
ROCm's exact pinned userspace and architecture-data closure is atomically
materialized under a content-addressed, owner-only cache. Its location follows
this precedence:

1. `LLM_CC_RUNTIME_DIR`
2. `$XDG_CACHE_HOME/llm-cc/runtime`
3. `$HOME/.cache/llm-cc/runtime`

CPU execution does not inspect or materialize either GPU payload.

```json
{"position":0,"token_id":785,"piece":"The","bytes_hex":"546865","probability":null,"log_probability":null,"entropy":null}
```

The analyzer calls the same scorer in-process; no subprocess or second runtime
is involved.

## Hermetic Linux build

Linux x86-64 builds use checksum-pinned Clang 22 and a Debian Stretch glibc
2.24 sysroot by default. The shipped ABI contract remains glibc 2.28 or newer;
CI rejects any imported symbol above that ceiling. The `portable` profile is
retained as an explicit alias:

```sh
bazel build --config=release --config=portable //dist:linux_x86_64
tools/check_glibc_version.sh bazel-bin/dist/llm-cc-0.1-linux-x86_64 2.28
tools/check_static_link.sh bazel-bin/dist/llm-cc-0.1-linux-x86_64
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

Repository: [pawelchcki/llm-cc](https://github.com/pawelchcki/llm-cc)
