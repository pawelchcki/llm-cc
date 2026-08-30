# LM-CC analyzer

Run the analyzer with a source file and either precomputed entropy JSONL or a
model. When neither `--entropy-jsonl` nor `--model` is supplied, `lmcc` defaults
to `models/DeepSeek-Coder-V2-Lite-Base-Q6_K.gguf` relative to the current
directory. Let `lmcc` download the recommended model when it is missing with:

```sh
lmcc path/to/source.cpp --download
```

The download is written to a `.partial` file and can be resumed by running the
same command again. To fetch the weights manually instead, use:

```sh
mkdir -p models && curl -L --fail -o models/DeepSeek-Coder-V2-Lite-Base-Q6_K.gguf https://huggingface.co/bartowski/DeepSeek-Coder-V2-Lite-Base-GGUF/resolve/main/DeepSeek-Coder-V2-Lite-Base-Q6_K.gguf
```

DeepSeek-Coder-V2-Lite-Base Q6_K is about 14.1 GB. Its mixture-of-experts
architecture uses about 2.4B active parameters, making CPU inference viable
with enough RAM. DeepSeek-Coder-6.7B-Base Q6_K and
Qwen2.5-Coder-1.5B-Base Q6_K are smaller fallbacks for lower-RAM machines.

The language is inferred from `.rs`, `.c`, `.h`, `.cc`, `.cpp`, `.cxx`, `.hpp`,
and `.hh` extensions:

```sh
cargo run -p lmcc-cli -- path/to/source.cpp --entropy-jsonl entropy.jsonl
```

Use `--lang rust`, `--lang c`, or `--lang cpp` to override inference;
`--lang c++` is an alias for `cpp`.

To run the C++ scorer, pass its binary; override `--model` only when using a
different GGUF. GPU and context options are forwarded only in this mode:

```sh
cargo run -p lmcc-cli -- path/to/source.rs \
  --scorer-bin ../bazel-bin/rethink-cc \
  --gpu-layers 99 \
  --context 8192
```

`--gpu-layers` and `--context` cannot be combined with `--entropy-jsonl`
because no scorer process runs in that mode.
