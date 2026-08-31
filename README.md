# llm-cc

`llm-cc` computes entropy-guided language-model code complexity (LM-CC) for
Rust, C, and C++. It is one C++20 binary built entirely with Bazel.

The analyzer removes comments with tree-sitter, obtains teacher-forced token
entropy from a llama.cpp-compatible GGUF (or checked-in JSONL), detects semantic
boundaries, builds the paper's compositional hierarchy, and prints JSON.

## Build

Bazelisk reads the pinned Bazel version automatically:

```sh
bazel build --config=release //:llm-cc
bazel test --config=cpu //...
```

The build downloads checksum-pinned LLVM, llama.cpp, tree-sitter and its Rust,
C, and C++ source bundles, nlohmann/json, curl, and OpenSSL. The default backend
is ROCm on Linux and Metal on macOS. Select a backend explicitly when building
for a different machine:

```sh
bazel build --config=release --config=rocm //:llm-cc
bazel build --config=release --config=cuda //:llm-cc
bazel build --config=release --config=metal //:llm-cc
bazel build --config=release --config=cpu //:llm-cc
```

ROCm builds download AMD's checksum-pinned TheRock 7.14.0 `gfx110X` SDK and
compile deterministic code objects for `gfx1100`, `gfx1101`, and `gfx1102`.
CUDA builds assemble a checksum-pinned CUDA 13.0.2 subset from NVIDIA's
redistributable component archives and use a pinned GCC 12.3 host compiler,
fixed architectures, and compiler random seeds. Linux C/C++ builds share that
compiler bundle's glibc 2.37 sysroot. Neither backend searches the host for a
vendor SDK, compiler, C library, or GNU Make.

HIP sources receive repository-relative deterministic compilation-unit IDs;
HIP/CUDA host paths are prefix-mapped and optional host ccache discovery is
disabled. This keeps sandbox locations out of accelerator artifacts.

Metal builds embed the Metal source in the binary instead of producing a
machine-specific `.metallib`. `--config=metal` pins Xcode 16.4 and a macOS 14.0
deployment target. Xcode and the Apple SDK are licensed host prerequisites, so
this boundary is deterministic but cannot be downloaded hermetically by the
repository.

The ROCm archive targets Linux x86_64 Radeon `gfx110X`. Use `--config=cpu` on
other Linux platforms, or change the pinned TheRock artifact and `GPU_TARGETS`
together when adding another AMD GPU family.

Install to `$HOME/.local/bin`, or choose another prefix:

```sh
bazel run //:install
bazel run //:install -- --prefix /opt/llm-cc
```

Accelerator installs keep the executable and its content-addressed Bazel
runfiles payload under `libexec/llm-cc`; the `bin/llm-cc` launcher selects that
immutable payload. This preserves the pinned vendor runtime after installation.

## Analyze source

The language is inferred from the file extension. Use `--lang rust|c|cpp` to
override it:

```sh
llm-cc source.cpp --model /path/to/model.gguf
llm-cc source.rs --entropy-jsonl entropy.jsonl
```

Analysis options are:

```text
--lang rust|c|cpp
--entropy-jsonl PATH
--model GGUF
--no-download
--gpu-layers N
--context N
--tau-percentile N
--alpha N
```

The default maximum input context is 131,072 tokens. The runtime allocates the
KV cache for the tokenized input rather than eagerly reserving the entire
maximum, so short source files retain a small memory footprint. Use `--context`
to select a different limit.

Without `--model` or `--entropy-jsonl`, `llm-cc` first checks
`models/DeepSeek-Coder-V2-Lite-Base-Q6_K.gguf`, then its model cache. If the
default model is absent it downloads it over HTTPS to a `.partial` file and
resumes that file on the next attempt. Disable this with `--no-download`.

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
`--context-size`, `--threads`, `--gpu-layers`, `--override-memory-check`, and
`--entropy`. It emits one JSONL object per observed token. It never samples or
generates a continuation. Its default maximum input context is 131,072 tokens;
use `--context-size` to override it. Mean negative log-likelihood and perplexity
go to stderr.

```json
{"position":0,"token_id":785,"piece":"The","bytes_hex":"546865","probability":null,"log_probability":null,"entropy":null}
```

The analyzer calls the same scorer in-process; no subprocess or second runtime
is involved.

## Hermetic Linux build

Linux x86_64 builds use the checksum-pinned LLVM, GCC, and glibc sysroot by
default. The `portable` profile remains as an explicit alias:

```sh
bazel build --config=release --config=portable --config=cpu //:llm-cc
tools/check_glibc_version.sh bazel-bin/llm-cc 2.37
tools/check_static_link.sh bazel-bin/llm-cc
```

The hermetic build uses static OpenSSL and curl for model downloads. The CA
bundle is selected from `SSL_CERT_FILE` first, followed by common Debian,
Fedora/RHEL, SUSE, and extracted trust-store paths.

## Development

```sh
tools/check_format.sh
tools/run_clang_tidy.sh
```

The project intentionally uses small standalone C++ tests instead of a test
framework. CI runs the full Bazel test suite and the portable release gates.

Repository: [pawelchcki/llm-cc](https://github.com/pawelchcki/llm-cc)
