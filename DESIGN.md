# LM-CC Design — implementing arXiv 2602.07882

Goal: implement **LM-CC** ("Rethinking Code Complexity Through the Lens of
Large Language Models", Xie et al., ICML 2026) faithfully, for multiple
analyzed languages in the future, starting with **Rust** as the first
language frontend.

## Paper recap (normative — implement to the letter)

LM-CC measures model-perceived code complexity via an entropy-guided
semantic compositional hierarchy.

1. **Preprocessing** — remove comments and docstrings from the source.
2. **Token entropy** — for each token position i, compute full-vocabulary
   next-token entropy under a pretrained LM:
   `H(t_i) = − Σ_j p(t_j | t_<i) · ln p(t_j | t_<i)`
3. **Boundary detection** — mark a semantic-unit boundary where
   `H(t_i) > τ` (τ = the **67th percentile** of the token-entropy
   distribution of the file, configurable), OR at explicit structural
   delimiters (termination of loops, conditionals, function definitions /
   scope changes), supplied by the language frontend.
4. **Hierarchy construction (Algorithm 1)** — partition the token stream
   into semantic units at boundaries; build a compositional tree by BFS,
   recursively partitioning each unit's range by nesting
   (indentation/syntactic structure) and enqueueing sub-units at the next
   level. For each unit v: `d(v)` = compositional level (nesting depth,
   root units at level 1), `b(v)` = number of children of v.
5. **Score** —
   `LM-CC = Σ_v [ α·b(v) + (1−α)·d(v) ]`, α = **0.8** default
   (equivalently α·TotalBranch + (1−α)·TotalCompLevel). No normalization.

## Architecture

Two components, clean separation between model inference and analysis:

### 1. C++ scorer extension (`src/`, existing Bazel target `//:rethink-cc`)

Add `--entropy` flag: for each position, in addition to the observed-token
probability, emit `"entropy"` — the natural-log entropy of the full
softmax distribution at that position. Computed in a single streaming pass
over the vocabulary row (log-sum-exp stabilized: with logits z_j,
`H = ln Σe^{z_j−m} − (Σ (z_j−m)·e^{z_j−m}) / Σe^{z_j−m}` where
m = max z). Keep bounded memory (one row at a time, as today). The entropy
at JSONL `position` k is H over the distribution that *predicts* token k
(i.e., aligned with the existing `probability` field; first token without
prefix gets `null`). Existing numeric-test pattern in `scoring_test.cc`
gets an entropy test (hand-computed small distributions).

### 2. Rust analyzer (`lmcc/` — Cargo workspace; Bazel integration deferred)

Crates:
- `lmcc-core` — model-agnostic pipeline: boundary detection, Algorithm 1
  hierarchy construction, scoring. Consumes:
  (a) a token stream with byte offsets + entropies, and
  (b) a list of structural-delimiter byte offsets + nesting depths from a
  language frontend.
- `lmcc-lang` — trait `LanguageFrontend { fn strip_comments(&self, src) -> (String, offset_map); fn structural_events(&self, src) -> Vec<StructuralEvent> }`
  with first impl `RustFrontend` using **tree-sitter + tree-sitter-rust**
  (comments/doc-comments removal preserving byte-offset mapping; block
  ends of `fn`, `loop`/`while`/`for`, `if`/`match`, and scope `{}`
  terminations as delimiters; node depth for nesting).
- `lmcc-cli` (binary `lmcc`) — pipeline driver:
  1. read source file (`--lang rust`, only rust for now),
  2. preprocess via frontend,
  3. obtain entropies: either spawn `rethink-cc --entropy --model <gguf> --file <preprocessed>`
     (`--scorer-bin`, `--model` flags) or read a precomputed JSONL
     (`--entropy-jsonl`) — the latter enables model-free testing,
  4. map token byte-ranges back to preprocessed source, detect boundaries
     (τ percentile via `--tau-percentile`, default 67; `--alpha`, default 0.8),
  5. build hierarchy, output JSON: `{ "lmcc": f64, "total_branch": u64,
     "total_comp_level": u64, "alpha": f64, "tau": f64, "units": [tree with
     byte ranges, level d(v), branching b(v)] }`.

Token↔source alignment uses the JSONL `bytes_hex` field: tokens are
concatenated in order and matched against the preprocessed source bytes.

## Testing strategy
- C++: entropy math against hand-computed distributions (`bazel test //...`).
- Rust: unit tests for percentile/boundary logic, Algorithm 1 on synthetic
  token streams with known trees, RustFrontend comment stripping and
  delimiter extraction on fixture files, end-to-end CLI test using a
  checked-in fixture entropy JSONL (no model download in CI).
- `cargo test` must pass; `cargo clippy -- -D warnings` clean.

### Interpretation notes

Semantic units are atomic: they are created only by boundaries (entropy
spikes and structural terminations), exactly as in the paper. Algorithm
1's "recursive partitioning by nesting" operates over the flat unit
sequence — each unit carries one nesting depth, sampled from the deepest
scope containing its first byte, and the hierarchy groups units by
recursively splitting depth runs. Nesting transitions strictly inside a
unit (a scope that opens and does not terminate before the unit's end)
do not create sub-units; a scope termination always ends a unit, so in
practice a unit never outlives the scopes it starts in.

## C and C++ frontends

`lmcc-lang` grows `CFrontend` and `CppFrontend` (tree-sitter-c /
tree-sitter-cpp), sharing the generic comment-stripping and
structural-event walkers with `RustFrontend` — the frontends differ only
in the tree-sitter language and two node-kind sets:

- **Comment kinds** — `comment` (both grammars cover `//` and `/* */`;
  doc comments are ordinary comments in C/C++).
- **Structural kinds** —
  - C: `function_definition`, `compound_statement`, `for_statement`,
    `while_statement`, `do_statement`, `if_statement`, `switch_statement`.
  - C++: all of C plus `for_range_loop`, `lambda_expression`,
    `try_statement`, `namespace_definition`, and the body scopes
    `field_declaration_list` (class/struct/union) and `enumerator_list`.

The `LanguageFrontend` trait, offset-map contract, and boundary semantics
are unchanged. `lmcc --lang` accepts `rust`, `c`, `cpp` (alias `c++`);
when `--lang` is omitted it is inferred from the source extension
(`.rs`; `.c`; `.cc`/`.cpp`/`.cxx`/`.hpp`/`.hh`; `.h` defaults to `c`).
Preprocessor directives are left in place (they are tokens the model
sees, exactly like any other code). Tests mirror the Rust suite: comment
stripping (including comment-like strings and stringized macros),
structural events and nesting on fixture files, and an end-to-end CLI run
against a checked-in fixture entropy JSONL for a C++ sample.

## Static linking

The CPU `rethink-cc` binary must depend only on the loader plus libc
family libraries (`libc`, `libm`, `libpthread`, `libdl`, `ld-linux`).
llama.cpp is already linked statically in the CPU config; the C++
runtime is linked statically too (`-static-libstdc++ -static-libgcc`,
or the toolchain's static libc++). GPU configs (CUDA/ROCm/Metal)
necessarily load vendor runtimes and are exempt. The `lmcc` Rust binary
already meets the same bar by default. A CI check (`ldd` allowlist over
the release CPU binary) enforces this.

## Out of scope for now
Evaluation harness (pass@1 correlation study), semantics-preserving
rewriting, other language frontends — the trait boundary is the extension
point.
