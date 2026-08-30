# rethink-cc

`rethink-cc` is a small Rust CLI for extracting teacher-forced token
probabilities from a local GGUF language model. It evaluates only the supplied
text; it does not sample or generate a continuation.

The default model is
[Qwen3.5-9B](https://huggingface.co/Qwen/Qwen3.5-9B), using Bartowski's
[Q4_K_M GGUF](https://huggingface.co/bartowski/Qwen_Qwen3.5-9B-GGUF). The
quantized model is about 6.2 GB and is downloaded into the Hugging Face cache
on first use. A Qwen3.5-4B Q4_K_M preset (about 3.0 GB) is included for
CPU-oriented use. You can supply any llama.cpp-compatible GGUF with `--model`.

## Build

You need a current Rust toolchain, CMake, a C/C++ compiler, libclang (for
bindgen), and OpenMP. On Debian or Ubuntu, the native dependencies are:

```sh
sudo apt-get install build-essential cmake libclang-dev libomp-dev
```

```sh
cargo build --release
```

Optional accelerator builds:

```sh
cargo build --release --features cuda
cargo build --release --features metal
cargo build --release --features rocm
cargo build --release --features vulkan
```

The default build uses CPU inference. CUDA targets NVIDIA, ROCm targets AMD,
and Metal targets Apple silicon. Backend toolchains must already be installed;
for example, a ROCm build requires the ROCm SDK and a CUDA build requires the
CUDA toolkit.

## Run

Score text with the default model:

```sh
cargo run --release -- --prompt "The quick brown fox"
```

Use the smaller model on a CPU-only machine:

```sh
cargo run --release -- --preset qwen-4b --threads 8 --prompt "The quick brown fox"
```

Use a model already on disk and offload its layers to a supported GPU backend:

```sh
cargo run --release --features cuda -- \
  --model /path/to/model.gguf \
  --gpu-layers 999 \
  --prompt "The quick brown fox"
```

The same runtime flags apply to ROCm and Metal builds:

```sh
cargo run --release --features rocm -- --gpu-layers 999 --prompt "Hello"
cargo run --release --features metal -- --gpu-layers 999 --prompt "Hello"
```

Text can also come from a file or standard input:

```sh
cargo run --release -- --file sample.txt
printf 'The quick brown fox' | cargo run --release --
```

Each standard-output line is a JSON object:

```json
{"bytes_hex":"546865","log_probability":null,"piece":"The","position":0,"probability":null,"token_id":785}
{"bytes_hex":"20717569636b","log_probability":-3.12,"piece":" quick","position":1,"probability":0.044,"token_id":3991}
```

`probability` is the softmax probability assigned to that observed token by the
model after ingesting the preceding tokens. `log_probability` is its natural
logarithm. `bytes_hex` preserves the exact token bytes when a token is not valid
UTF-8 by itself. Aggregate mean negative log-likelihood and perplexity are
written to standard error.

Causal models cannot score the first token without a prefix. Consequently its
probability is `null` by default. Use `--add-bos` for models whose tokenizer is
trained with a beginning-of-stream token; Qwen3.5 normally does not use one.

Run `cargo run --release -- --help` for all options.

## How scoring works

For token sequence `t0, t1, ...`, the logits produced after ingesting `t0`
predict `t1`. The CLI evaluates one observed token at a time, applies a stable
log-softmax to the full vocabulary, and selects the entry for the next observed
token. This keeps logit memory bounded to one vocabulary row and never enters a
generation loop.

## License

MIT
