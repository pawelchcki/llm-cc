# llm-cc design

`llm-cc` is a single C++20 executable with one Bazel build graph. The binary
has three entry points:

- default analysis: `llm-cc SOURCE ...`
- raw teacher-forced scoring: `llm-cc score ...`
- cached model management: `llm-cc models ...`

## Analysis pipeline

1. `src/lang.cc` parses Rust, C, C++, CUDA `.cu`/`.cuh`, Java, Python, Go,
   JavaScript, or C#
   through tree-sitter's C API. Exhaustive language metadata owns each
   grammar, canonical name, aliases, extensions, comment nodes, structural
   nodes, and callable kinds. One parse collects comments, Python docstrings,
   callable spans, meaningful lines, and structural scopes. Removal preserves
   newlines and an original-byte map; metadata is remapped instead of reparsing
   potentially invalid empty Python suites. JavaScript covers Node.js `.js`,
   `.mjs`, and `.cjs` files, not JSX or TypeScript.
2. `src/score_cmd.cc` loads a GGUF with llama.cpp and teacher-forces the
   preprocessed source through configurable batches. Analysis consumes native
   records through an in-process sink; `llm-cc score` exposes the byte-exact
   JSONL form for interoperability and debugging. Contexts are reused, grow
   geometrically, and are discarded after inference failures.
   Optional throttled progress callbacks report completed batches without
   changing inference or stdout JSONL.
3. `src/jsonl.cc` parses entropy records, reconstructs token bytes from
   `bytes_hex`, verifies contiguous positions and exact source coverage, and
   aligns entropy to preprocessed byte ranges.
4. `src/core.cc` applies an absolute tau by default (or resolves an explicitly
   requested percentile). Structural mode combines line-snapped entropy and
   scope-termination boundaries. Reference mode uses entropy-led logical-line
   partitioning. Both use source-aware meaningful positions and a scope sweep.
   One implicit root occupies level 1; an undivided nonempty input is root-only
   and scores `1 - alpha`.
5. Unit offsets are mapped back to the original source and emitted as pretty
   JSON. The top-level score field is `llm_cc`.

## Scoring contract

For observed tokens `t[i]`, logits produced after `t[i]` score `t[i+1]`.
Without a beginning-of-stream token, the first observed token has null
probability, log probability, and entropy. Host reduction retains logits for
the current batch; reduced device execution does not allocate a raw host-logits
buffer. Token pieces have both a readable JSON string and exact `bytes_hex`,
so invalid standalone UTF-8 pieces remain lossless.

Host reduction is the double-precision numerical reference. Device reduction
is a llama backend-sampler graph placed after final logits: stable max-shifted
operations return entropy and observed-token log probability per row. The
existing CUDA/HIP and Metal implementations execute those ggml operations;
only the two-result tensor is copied to the host, and raw logits storage/copies
are suppressed. Invalid results and token IDs are checked before emission.

Before loading a model, the scorer checks host memory and, when requested, GPU
memory. The estimate is the model file plus a 10% weight margin and 512 MiB of
context overhead. `--override-memory-check` bypasses this heuristic.

GPU inference holds a per-user, per-backend advisory lock for the model
lifetime. Concurrent `llm-cc` processes therefore queue before measuring free
device memory or loading weights. A zero-memory result inside a detected Codex
sandbox gets a targeted diagnostic because sandboxed Metal accounting can hide
the accelerator.

## Model lifecycle

`src/cache.cc` owns cache selection, `models.json`, timestamps, listing, and
safe removal of bare file names. `src/download.cc` owns HTTPS downloads through
static libcurl/OpenSSL. Downloads write `MODEL.partial`, request a byte range
when that file exists, validate the HTTP response, and rename only after a
complete transfer.

## Build boundaries

All third-party source is checksum-pinned in `MODULE.bazel`. tree-sitter uses
the runtime unity C source and official complete-source grammar bundles.
llama, ggml CPU, CUDA, HIP, and Metal are native per-source Bazel actions.
Only stable leaf dependencies such as OpenSSL and curl retain foreign builds.

Linux development keeps GPU modules in runfiles. The release statically links
the application and CPU stack, exports only the small ggml backend ABI, and
appends deterministic compressed CUDA and ROCm bundles. CUDA is loaded from a
sealed memfd; ROCm is extracted into a content-addressed private cache only
when selected. CPU execution does not touch either payload.

The default Linux toolchain is pinned Clang with a glibc 2.24 sysroot; the
`portable` profile is an alias for the same hermetic configuration. CI enforces
the public glibc 2.28 ceiling and rejects project or GPU userspace dynamic
dependencies from the shipped ELF.
