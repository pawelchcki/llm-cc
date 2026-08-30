# rethink-cc

`rethink-cc` is a small C++20 CLI that extracts teacher-forced probabilities
for the tokens in supplied text. It loads a llama.cpp-compatible GGUF, evaluates
only the input, and emits JSONL; it never samples or generates a continuation.

The build is pinned to Bazel 9.2.0 and uses a Bazel-downloaded LLVM 22.1.7 C/C++
toolchain. llama.cpp, CMake, and Ninja are checksum/version pinned as Bazel
dependencies. CUDA, ROCm, and Apple Metal builds still require the corresponding
vendor SDK and driver on the target machine.

## Model

The recommended 5–10B default is Qwen3.5-9B Q4_K_M (about 6.2 GB). Download it
outside the build graph so model weights remain a runtime input:

```sh
mkdir -p models
curl -L --fail \
  -o models/Qwen3.5-9B-Q4_K_M.gguf \
  https://huggingface.co/bartowski/Qwen_Qwen3.5-9B-GGUF/resolve/main/Qwen3.5-9B-Q4_K_M.gguf
```

For CPU-only machines, Qwen3.5-4B Q4_K_M is about 3.0 GB:

```sh
mkdir -p models
curl -L --fail \
  -o models/Qwen3.5-4B-Q4_K_M.gguf \
  https://huggingface.co/bartowski/Qwen_Qwen3.5-4B-GGUF/resolve/main/Qwen_Qwen3.5-4B-Q4_K_M.gguf
```

Any GGUF supported by the pinned llama.cpp revision can be used.

## Build and run

Install Bazelisk (recommended), then build the CPU backend. Bazelisk reads the
pinned `.bazelversion` automatically.

```sh
bazel build --config=release --config=cpu //:rethink-cc
bazel-bin/rethink-cc \
  --model models/Qwen3.5-9B-Q4_K_M.gguf \
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

# AMD ROCm SDK and driver required
bazel build --config=release --config=rocm //:rethink-cc

# macOS with Xcode command-line tools required
bazel build --config=release --config=metal //:rethink-cc
```

Offload all transformer layers at runtime with `--gpu-layers -1`. Use zero
(the default) for CPU inference.

```sh
bazel-bin/rethink-cc \
  --model models/Qwen3.5-9B-Q4_K_M.gguf \
  --gpu-layers -1 \
  --prompt "The quick brown fox"
```

Run the model-independent numerical and encoding tests with:

```sh
bazel test //...
```

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

The `lmcc/` Cargo workspace implements entropy-guided LM-CC for Rust. It strips
comments with tree-sitter-rust, detects entropy and Rust structural boundaries,
constructs the semantic compositional hierarchy, and emits the score and tree as
JSON.

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
  --scorer-bin bazel-bin/rethink-cc \
  --model models/Qwen3.5-9B-Q4_K_M.gguf
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
