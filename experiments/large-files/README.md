# Large-file preprocessing and windowed scoring

The source limit is 1 GiB (1,073,741,824 bytes). This is an input refusal
ceiling, not a promise that a 1 GiB syntax tree fits in ordinary workstation
memory. Parsing and preprocessing use a 300-second cooperative deadline and
1,024-level syntax/structural depth limits. Model tokenization and neural
scoring have separate, hardware-dependent costs.

## Reproducing the preprocessing measurements

Build the optimized CPU stress runner, then invoke it directly so build time
does not enter the measurement:

```sh
bazel build --config=cpu -c opt //:lang_stress
timeout 360s bazel-bin/lang_stress 100 cpp representative
timeout 360s bazel-bin/lang_stress 100 python representative
```

Arguments are `MiB`, language, and scenario (`representative`, `wide`,
`malformed`, or `deep`). Representative inputs consist of distinct small
functions containing loops and branches, with complete declarations until
the requested byte count is reached. They contain real syntax throughout;
they are not padded mostly with comments or whitespace. The runner checks
the expected number of functions and complete original-offset coverage.
Its timing includes tree construction, metadata extraction, comment
preprocessing, and tree destruction, but excludes fixture generation.
`peak_rss_bytes` includes the generated source and the process high-water mark.

Measurements below were obtained on 2026-09-13 on Linux x86-64, an AMD Ryzen
9 7900, and 124 GiB RAM, using the repository's optimized hermetic Clang build.
Some runs overlapped other test processes, so these are observed completion
times, not isolated microbenchmarks or universal latency guarantees.

| Input | Language | Functions | Preprocessing seconds | Peak RSS, GiB |
| --- | --- | ---: | ---: | ---: |
| 10 MiB | C++ | 137,622 | 3.65 | 0.84 |
| 10 MiB | C | 141,292 | 3.39 | 0.82 |
| 10 MiB | Rust | 153,578 | 3.62 | 0.96 |
| 10 MiB | Java | 126,154 | 3.75 | 0.95 |
| 10 MiB | Python | 110,385 | 2.67 | 0.63 |
| 10 MiB | Go | 147,179 | 3.47 | 0.89 |
| 10 MiB | JavaScript | 134,138 | 3.41 | 0.86 |
| 10 MiB | C# | 126,154 | 4.74 | 1.25 |
| 100 MiB | C++ | 1,358,574 | 51.23 | 8.21 |
| 100 MiB | Python | 1,092,461 | 35.70 | 6.15 |

Each representative input retained a single compact offset-map span. The
complete tree remains the dominant host-memory cost. Both 100 MiB runs met
the default preprocessing budget, but their peak memory makes a machine with
substantial free RAM necessary. Memory consumption still scales with syntax,
source bytes, and token count.

A 100 MiB C++ array with over fifty million scalar elements also completed,
in 62.70 seconds at 15.73 GiB peak RSS. This wide-tree stress case guards
against repeated indexed child lookups and unnecessary cursor field lookups.
A 4,096-parenthesis expression was
rejected with the explicit depth-limit diagnostic. Unit coverage exercises
an actual tree-sitter cancellation with a positive tiny time budget, rather
than only rejecting an already-expired deadline before parsing.

The `//:large_file_parse_test` regression checks a dense 10 MiB C++ input as
part of `//:unit`, with an external Bazel test timeout in addition to the
cooperative parser deadline. The 100 MiB cases remain explicit stress runs
because of their RAM requirements.

## Scoring validation and interpretation

```sh
bazel test --config=cpu -c opt //:model_smoke_test
```

The real-model test uses the pinned Qwen 0.5B GGUF. It scores a source across
more than two windows with an odd context size of 25 and batch size of 7,
with and without a synthetic BOS token. It checks exact source bytes,
contiguous positions, no extra null scores, and probability/entropy agreement
with independent first, middle, and final window evaluations.

Window planning uses constant space. Inference recomputes the overlap to
initialize each window's K/V state, but requests logits and entropy only for
new targets. Every source token appears once in the output. Syntax scopes and
LM-CC aggregation cover the entire file. The original no-BOS first-token
null score remains part of the existing scoring contract.

These checks establish complete coverage and correct window semantics; they
do not establish equivalence with unlimited-context entropy, validate a new
penalized complexity metric, or measure end-to-end neural scoring of a
100 MiB file. Context overflow is reported separately relative to a fixed
128K-token reference. GPU execution requires its own backend validation;
the measurements above use CPU execution and CPU syntax parsing.
