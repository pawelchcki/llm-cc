# LM-CC analyzer

Run the analyzer with a source file and either precomputed entropy JSONL or a
model. The language is inferred from `.rs`, `.c`, `.h`, `.cc`, `.cpp`, `.cxx`,
`.hpp`, and `.hh` extensions:

```sh
cargo run -p lmcc-cli -- path/to/source.cpp --entropy-jsonl entropy.jsonl
```

Use `--lang rust`, `--lang c`, or `--lang cpp` to override inference;
`--lang c++` is an alias for `cpp`.

To run the C++ scorer directly, pass a GGUF model and scorer binary. GPU and
context options are forwarded only in this mode:

```sh
cargo run -p lmcc-cli -- path/to/source.rs \
  --model ../models/deepseek-coder-6.7b-base.Q6_K.gguf \
  --scorer-bin ../bazel-bin/rethink-cc \
  --gpu-layers 99 \
  --context 8192
```

`--gpu-layers` and `--context` cannot be combined with `--entropy-jsonl`
because no scorer process runs in that mode.
