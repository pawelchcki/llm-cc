# rethink-cc

`rethink-cc` is a small C++20 CLI that extracts teacher-forced probabilities
for the tokens in supplied text. It loads a llama.cpp-compatible GGUF, evaluates
only the input, and emits JSONL; it never samples or generates a continuation.

The build is pinned to Bazel 9.2.0 and uses a Bazel-downloaded LLVM 22.1.7 C/C++
toolchain. llama.cpp, CMake, and Ninja are checksum/version pinned as Bazel
dependencies. CUDA, ROCm, and Apple Metal builds still require the corresponding
vendor SDK and driver on the target machine.

## Model

The primary recommendation is the pretrained DeepSeek-Coder-V2-Lite-Base Q6_K
(about 14.1 GB). It is a mixture-of-experts model with about 2.4B active
parameters, so CPU inference is viable when the machine has enough RAM.
Model weights stay outside the build graph as a runtime input. The Rust `lmcc`
CLI can download the default model itself when it is missing:

```sh
lmcc path/to/source.cpp --download
```

The download is written to a `.partial` file and can be resumed by running the
same command again. Alternatively, fetch the weights manually with curl:

```sh
mkdir -p models && curl -L --fail -o models/DeepSeek-Coder-V2-Lite-Base-Q6_K.gguf https://huggingface.co/bartowski/DeepSeek-Coder-V2-Lite-Base-GGUF/resolve/main/DeepSeek-Coder-V2-Lite-Base-Q6_K.gguf
```

When `lmcc` runs model inference without an explicit `--model`, it uses
`models/DeepSeek-Coder-V2-Lite-Base-Q6_K.gguf` relative to the current
directory.

For lower-RAM machines, DeepSeek-Coder-6.7B-Base Q6_K (about 5.5 GB) is the
larger fallback:

```sh
mkdir -p models
curl -L --fail \
  -o models/deepseek-coder-6.7b-base.Q6_K.gguf \
  https://huggingface.co/TheBloke/deepseek-coder-6.7B-base-GGUF/resolve/main/deepseek-coder-6.7b-base.Q6_K.gguf
```

For a small CPU option, use Qwen2.5-Coder-1.5B Base Q6_K (about 1.2 GB):

```sh
mkdir -p models
curl -L --fail \
  -o models/Qwen2.5-Coder-1.5B-Q6_K.gguf \
  https://huggingface.co/tensorblock/Qwen2.5-Coder-1.5B-GGUF/resolve/main/Qwen2.5-Coder-1.5B-Q6_K.gguf
```

LM-CC measures the model's raw predictive uncertainty, so pretrained base code
models are better entropy estimators than post-trained chat models, whose
instruction tuning reshapes the next-token distribution. Q6_K or Q8_0 also
preserves that distribution more faithfully than Q4; prefer Q8_0 when memory is
plentiful. Any GGUF supported by the pinned llama.cpp revision can be used.

## Build and run

Install Bazelisk (recommended), then build the CPU backend. Bazelisk reads the
pinned `.bazelversion` automatically.

```sh
bazel build --config=release --config=cpu //:rethink-cc
bazel-bin/rethink-cc \
  --model models/DeepSeek-Coder-V2-Lite-Base-Q6_K.gguf \
  --entropy \
  --prompt "The quick brown fox"
```

Text can come from a file or standard input:

```sh
bazel-bin/rethink-cc --model /path/to/model.gguf --file sample.txt
printf 'The quick brown fox' | bazel-bin/rethink-cc --model /path/to/model.gguf
```

Accelerator configurations compile the corresponding llama.cpp backend:

```sh
# NVIDIA CUDA toolkit and driver required
bazel build --config=release --config=cuda //:rethink-cc

# AMD ROCm SDK and driver required; see the environment setup below
bazel build --config=release --config=rocm //:rethink-cc

# macOS with Xcode command-line tools required
bazel build --config=release --config=metal //:rethink-cc
```

ROCm builds require `ROCM_PATH`/`HIP_PATH` to name the SDK root and `HIPCXX` to
name its `llvm/bin/clang++`. `GPU_TARGETS` is optional; set it to the
architecture from `rocminfo` to avoid compiling for every detected GPU. The
ROCm Bazel profile forwards these variables into the CMake action and sets
`CMAKE_HIP_COMPILER` from `HIPCXX` explicitly.

```sh
export ROCM_PATH=/opt/rocm
export HIP_PATH="$ROCM_PATH"
export HIPCXX="$ROCM_PATH/llvm/bin/clang++"
export GPU_TARGETS=gfx1100
bazel build --config=release --config=rocm //:rethink-cc
```

Override the target for another build by changing `GPU_TARGETS` (for example,
`GPU_TARGETS=gfx1030 bazel build --config=rocm //:rethink-cc`). If it is unset,
llama.cpp detects the GPUs present on the build machine.

Offload all transformer layers at runtime with `--gpu-layers -1`. Use zero
(the default) for CPU inference.

```sh
bazel-bin/rethink-cc \
  --model models/DeepSeek-Coder-V2-Lite-Base-Q6_K.gguf \
  --gpu-layers -1 \
  --prompt "The quick brown fox"
```

Before loading a model, `rethink-cc` checks whether available memory can hold
the model file plus a 10% safety margin and an estimated 512 MiB context
overhead. With GPU offload, model weights are conservatively charged to the
first GPU and context overhead to host RAM. If this coarse preflight estimate
does not match a known-good setup, pass `--override-memory-check` to bypass it.

Run the model-independent numerical and encoding tests with:

```sh
bazel test //...
```

## C++ hygiene

The repository uses the clang-format and clang-tidy binaries from its pinned
Bazel LLVM toolchain. Check formatting with `tools/check_format.sh`, and run the
local static-analysis pass with `tools/run_clang_tidy.sh`. The format check also
runs in BuildBuddy CI; clang-tidy remains a developer check to keep CI costs
low. Both scripts inspect only C++ files under `src/`, never `third_party/`.

## CI

The primary CI definition is `buildbuddy.yaml`. After linking this repository
in BuildBuddy and enabling Workflows, it runs the CPU tests and release build on
Ubuntu 24.04 for pushes to `main`, pull requests targeting `main`, and GitHub
merge-queue branches. BuildBuddy supplies workflow authentication and build
event reporting; the `--config=buildbuddy` profile enables its shared remote
cache without storing a key in the repository.

GitHub Actions runs the same commands independently as a fallback, so a
BuildBuddy outage or incomplete initial account setup does not leave the branch
without CI coverage.

## Output

Each stdout line describes one observed input token:

```json
{"position":0,"token_id":785,"piece":"The","bytes_hex":"546865","probability":null,"log_probability":null,"entropy":null}
{"position":1,"token_id":3991,"piece":" quick","bytes_hex":"20717569636b","probability":0.044,"log_probability":-3.12,"entropy":5.71}
```

`probability` is the full-vocabulary softmax probability assigned to the
observed token after ingesting its prefix; `log_probability` is its natural
logarithm. `bytes_hex` preserves exact token bytes even when an individual piece
is not valid UTF-8. With `--entropy`, `entropy` is the natural-log entropy of the
full next-token distribution aligned with that observed token. The first token
without a prefix has `null` probability, log probability, and entropy. Mean
negative log-likelihood and perplexity go to stderr.

The default `--bos auto` follows the model tokenizer metadata. Without a BOS,
the first observed token has no prefix and therefore receives `null`. Override
this with `--bos always` or `--bos never`.

Internally, logits after token `t[i]` are used to score observed token `t[i+1]`.
Only one vocabulary row is retained at a time, so the CLI has no text-generation
loop and bounded logit memory.

## LM-CC analyzer

The `lmcc/` Cargo workspace implements entropy-guided LM-CC for Rust, C, and
C++. It strips comments with tree-sitter, detects entropy and structural
boundaries, constructs the semantic compositional hierarchy, and emits the
score and tree as JSON. The language is inferred from the file extension, or
passed explicitly with `--lang rust|c|cpp` (see `lmcc/README.md`).

Build and test it independently of Bazel:

```sh
cd lmcc
cargo build
cargo test
cargo clippy --workspace --all-targets -- -D warnings
```

Run the complete pipeline with a local GGUF and the C++ scorer:

```sh
lmcc/target/debug/lmcc path/to/source.rs \
  --lang rust \
  --scorer-bin bazel-bin/rethink-cc
```

When the scorer was built with CUDA, ROCm, or Metal, request layer offload in
the same pipeline with `--gpu-layers`; `--context` forwards the scorer's maximum
token context when a larger file needs it:

```sh
lmcc/target/debug/lmcc path/to/source.rs \
  --scorer-bin bazel-bin/rethink-cc \
  --gpu-layers 99 \
  --context 8192
```

For model-free/reproducible analysis, supply JSONL previously produced with
`rethink-cc --entropy`:

```sh
lmcc/target/debug/lmcc path/to/source.rs \
  --entropy-jsonl path/to/entropy.jsonl \
  --tau-percentile 67 \
  --alpha 0.8
```

Token bytes in the JSONL must concatenate exactly to the comment-stripped source.
The output has the following shape; byte ranges are mapped back to the original
source:

```json
{
  "lmcc": 1.4,
  "total_branch": 1,
  "total_comp_level": 3,
  "alpha": 0.8,
  "tau": 0.736,
  "units": [
    {
      "start_byte": 0,
      "end_byte": 29,
      "level": 1,
      "branching": 1,
      "children": [
        {
          "start_byte": 12,
          "end_byte": 29,
          "level": 2,
          "branching": 0,
          "children": []
        }
      ]
    }
  ]
}
```

## License

MIT
