# llm-cc

`llm-cc` computes entropy-guided language-model code complexity (LM-CC) for
Rust, C, and C++. It is one C++20 binary built entirely with Bazel.

The analyzer removes comments with tree-sitter, obtains teacher-forced token
entropy from a llama.cpp-compatible GGUF (or checked-in JSONL), detects semantic
boundaries, builds the paper's compositional hierarchy, and prints JSON.

## Build

Bazelisk reads the pinned Bazel version automatically:

```sh
bazel build --config=release --config=cpu //:llm-cc
bazel test //...
```

The build downloads checksum-pinned LLVM, llama.cpp, tree-sitter and its Rust,
C, and C++ source bundles, nlohmann/json, curl, and OpenSSL. CUDA, ROCm, and
Metal builds use `--config=cuda`, `--config=rocm`, and `--config=metal` and
require the corresponding vendor SDK.

Install to `$HOME/.local/bin`, or choose another prefix:

```sh
bazel run //:install
bazel run //:install -- --prefix /opt/llm-cc
```

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
generates a continuation. Mean negative log-likelihood and perplexity go to
stderr.

```json
{"position":0,"token_id":785,"piece":"The","bytes_hex":"546865","probability":null,"log_probability":null,"entropy":null}
```

The analyzer calls the same scorer in-process; no subprocess or second runtime
is involved.

## Portable Linux build

Linux x86_64 release binaries can target the glibc 2.17 compatibility floor:

```sh
bazel build --config=release --config=portable --config=cpu //:llm-cc
tools/check_glibc_version.sh bazel-bin/llm-cc
tools/check_static_link.sh bazel-bin/llm-cc
```

The portable build uses static OpenSSL and curl for model downloads. The CA
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
