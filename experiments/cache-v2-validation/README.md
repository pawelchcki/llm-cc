# Global entropy cache v2 validation

Validated on 2026-09-05 using the retained 26-file model-selection corpus and
both registered smaller alternatives. The original experiment and its weights
were only read. All changes remain uncommitted.

For both models, every file result with unchanged inference/scoring settings
matched the uncached result exactly, excluding the path and cache-hit flag.
Each reuse run returned 26/26 hits in the original repository, a linked Git
worktree, a separate repository, and a non-Git copy. Identical copied weights
also returned 26/26 hits after verifying their content. Changing tau, alpha,
and hierarchy together reused all 26 entries while recomputing scores.

| Model (Q6) | Uncached | Populate cache | Warm hits, two runs | Hits requiring first digest |
| --- | ---: | ---: | ---: | ---: |
| Qwen2.5-Coder 1.5B | 20.498 s | 28.710 s | 0.288 / 0.286 s | 8.083 s |
| Qwen2.5-Coder 3B | 22.877 s | 39.235 s | 0.271 / 0.286 s | 18.431 s |

These are whole-process times for all 26 files on one NVIDIA A10G, including
preprocessing and output. The first-digest measurement deletes only the isolated
test cache's digest memos, retaining entropy entries, so it includes hashing plus
ordinary cache-hit work. It is not a cold-disk benchmark. The two warm runs reuse
both digests and entropy. Downloads and build time are excluded.

The final shared cache contains 52 entries totaling 638,478 bytes, with no
malformed entries. Its reported limit is 1,073,741,824 bytes and retention is
1,728,000 seconds. Model digests match the independently retained checksums in
the model-selection manifest.

## Checks

- Local macOS: `bzl test //:unit //:cli_test --test_output=errors`, 20 targets pass.
- Packaging SHA integration: `bzl build //tools:embed_payloads` passes.
- Independent Linux: `cache_test`, `model_identity_test`, `entropy_cache_test`,
  and `cache_concurrency_test` pass against the reviewed implementation.
- Expiry is checked at exactly 20 days before refreshing timestamps. Tests cover
  refreshed recency, LRU order, oversized entries, failed eviction, interrupted
  and malformed accounting, abandoned entry/metadata temporaries, provenance,
  token coverage, and validated hits surviving maintenance/lock failures.
- Process tests overlap publication, reads, pruning, and clearing; verify one
  download per target, independent targets proceeding concurrently, model
  removal waiting for acquisition, and concurrent manifest updates retaining
  every new and unrelated record. Remaining entries decode as valid CBOR and
  accounting matches their actual sizes. The lock inode survives clearing.
- Identity tests include identical copies, same-size changes with restored
  modification time, inode replacement, malformed/unavailable memos, symlink
  refusal, private permissions, and continuously changing weights being rejected.

Windows branches were updated but were not executed in this validation.

## Build and artifacts

All first-party C++ implementation files were recompiled in an isolated Linux
directory with pinned Clang 22, libc++, and `-O2`. Existing pinned llama.cpp,
CUDA, and tree-sitter dependency objects were reused from the retained build.
The source hashes in [build-provenance.json](results/build-provenance.json)
were independently compared with this working tree; the binary hash is included.
The ordinary Bazel build graph was verified locally.

[summary.json](results/summary.json) records commands, totals, timings, and
validation outcomes. `results/` also contains compressed JSONL and stderr for
each run. Weights, source copies, and runtime caches are not included.

To repeat with a Linux CUDA build and a fresh output directory:

```sh
python3 tools/cache_validation.py \
  --study /path/to/retained-model-study \
  --binary /path/to/llm-cc \
  --output /path/to/new-cache-validation
```
