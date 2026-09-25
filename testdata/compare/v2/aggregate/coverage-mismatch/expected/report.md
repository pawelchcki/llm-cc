## llm-cc comparison

Status: **failed**

Scores and ranks use total LM-CC; LM-CC/token is secondary.

- runtime unavailable → unavailable LM-CC (unavailable)
- tests unavailable → unavailable LM-CC (unavailable)
- tooling unavailable → 0.0 LM-CC (unavailable)
- repository unavailable → unavailable LM-CC (unavailable)

| Category | LM-CC base | LM-CC head | LM-CC Δ | LM-CC Δ% | LM-CC/token Δ | Tokens Δ | Coverage |
|---|---:|---:|---:|---:|---:|---:|---:|
| runtime | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | 0.0% / 0.0% |
| tests | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | 0.0% / 0.0% |
| tooling | unavailable | 0.0 | unavailable | unavailable | unavailable | unavailable | 0.0% / 100.0% |
| repository | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | 0.0% / 0.0% |

Cache: 0 hits, 9 misses.

Errors:
- `worker 0 result coverage mismatch (missing 1, extra 0)`
- `unmeasured unique files: 9`

Rules: host

### Raw totals

| Side/category | LM-CC | Tokens | LM-CC/token |
|---|---:|---:|---:|
| base/runtime | unavailable | unavailable | unavailable |
| base/tests | unavailable | unavailable | unavailable |
| base/tooling | unavailable | unavailable | unavailable |
| head/runtime | unavailable | unavailable | unavailable |
| head/tests | unavailable | unavailable | unavailable |
| head/tooling | 0.0 | 0 | unavailable |

### Coverage details

- Base: 0/8 supported paths measured; unscored paths: symlink: 1, unsupported: 1
- Head: 0/9 supported paths measured; unscored paths: oversized: 1, symlink: 1, unsupported: 1

### Changed paths

- ∅ → `large.py` (A)
- `same.cc` → `same.cc` (M)
- `move.cc` → `tests/move.cc` (R100)
- `tools/gen.py` → ∅ (D)
- ∅ → `zero.go` (A)

### Base inventory

| Path | Category | Language | Bytes | Measurement |
|---|---|---|---:|---|
| `README.md` | runtime | — | 5 | unsupported |
| `api.hpp` | runtime | cpp | 11 | missing |
| `copy.cc` | runtime | cpp | 7 | missing |
| `link.cc` | runtime | cpp | 9 | symlink |
| `move.cc` | runtime | cpp | 10 | missing |
| `odd\u000aname.py` | runtime | python | 6 | missing |
| `same.cc` | runtime | cpp | 7 | missing |
| `src_test.cc` | tests | cpp | 10 | missing |
| `target.cc` | runtime | cpp | 9 | missing |
| `tools/gen.py` | tooling | python | 16 | missing |

### Head inventory

| Path | Category | Language | Bytes | Measurement |
|---|---|---|---:|---|
| `README.md` | runtime | — | 5 | unsupported |
| `api.hpp` | runtime | cpp | 11 | missing |
| `copy.cc` | runtime | cpp | 7 | missing |
| `large.py` | runtime | python | 120 | oversized |
| `link.cc` | runtime | cpp | 9 | symlink |
| `odd\u000aname.py` | runtime | python | 6 | missing |
| `same.cc` | runtime | cpp | 13 | missing |
| `src_test.cc` | tests | cpp | 10 | missing |
| `target.cc` | runtime | cpp | 9 | missing |
| `tests/move.cc` | tests | cpp | 10 | missing |
| `zero.go` | runtime | go | 13 | missing |
