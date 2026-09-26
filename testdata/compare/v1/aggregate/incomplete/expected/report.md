## llm-cc comparison

Status: **incomplete**

Scores and ranks use total LM-CC; LM-CC/token is secondary.

- runtime unavailable → unavailable LM-CC (unavailable)
- tests 10.5 → 12.0 LM-CC (+14.3%)
- tooling unavailable → 0.0 LM-CC (unavailable)
- repository unavailable → unavailable LM-CC (unavailable)

| Category | LM-CC base | LM-CC head | LM-CC Δ | LM-CC Δ% | LM-CC/token Δ | Tokens Δ | Coverage |
|---|---:|---:|---:|---:|---:|---:|---:|
| runtime | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | 83.3% / 42.9% |
| tests | 10.5 | 12.0 | +1.5 | +14.3% | -0.3 | 3 | 100.0% / 100.0% |
| tooling | unavailable | 0.0 | unavailable | unavailable | unavailable | unavailable | 0.0% / 100.0% |
| repository | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | 75.0% / 55.6% |

Cache: 0 hits, 5 misses.

### Changed files

| Path | Category | LM-CC base | LM-CC head | LM-CC Δ | LM-CC Δ% | Head rank |
|---|---|---:|---:|---:|---:|---:|
| `tests/move.cc` | tests | 1.5 | 1.5 | +0.0 | +0% | #5/5 |
| `same.cc` | runtime | 6.0 | unavailable | unavailable | unavailable | — |

<details>
<summary>Top offenders on the merge base (`1d43f119f327a404e8f9633434f83e55995f2369`)</summary>

| # | Path | Category | LM-CC | Touched |
|---:|---|---|---:|---|
| 1 | `src_test.cc` | tests | 10.5 |  |
| 2 | `odd\u000aname.py` | runtime | 8.2 |  |
| 3 | `copy.cc` | runtime | 6.0 |  |
| 4 | `same.cc` | runtime | 6.0 | yes |
| 5 | `target.cc` | runtime | 3.8 |  |
| 6 | `move.cc` | runtime | 1.5 | yes |

</details>

Rules: host

### Raw totals

| Side/category | LM-CC | Tokens | LM-CC/token |
|---|---:|---:|---:|
| base/runtime | unavailable | unavailable | unavailable |
| base/tests | 10.5 | 7 | 1.5 |
| base/tooling | unavailable | unavailable | unavailable |
| head/runtime | unavailable | unavailable | unavailable |
| head/tests | 12.0 | 10 | 1.2 |
| head/tooling | 0.0 | 0 | unavailable |

### Coverage details

- Base: 6/8 supported paths measured; unscored paths: oversized: 2, symlink: 1, unsupported: 1
- Head: 5/9 supported paths measured; unscored paths: oversized: 4, symlink: 1, unsupported: 1

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
| `api.hpp` | runtime | cpp | 11 | oversized |
| `copy.cc` | runtime | cpp | 7 | measured |
| `link.cc` | runtime | cpp | 9 | symlink |
| `move.cc` | runtime | cpp | 10 | measured |
| `odd\u000aname.py` | runtime | python | 6 | measured |
| `same.cc` | runtime | cpp | 7 | measured |
| `src_test.cc` | tests | cpp | 10 | measured |
| `target.cc` | runtime | cpp | 9 | measured |
| `tools/gen.py` | tooling | python | 16 | oversized |

### Head inventory

| Path | Category | Language | Bytes | Measurement |
|---|---|---|---:|---|
| `README.md` | runtime | — | 5 | unsupported |
| `api.hpp` | runtime | cpp | 11 | oversized |
| `copy.cc` | runtime | cpp | 7 | measured |
| `large.py` | runtime | python | 120 | oversized |
| `link.cc` | runtime | cpp | 9 | symlink |
| `odd\u000aname.py` | runtime | python | 6 | measured |
| `same.cc` | runtime | cpp | 13 | oversized |
| `src_test.cc` | tests | cpp | 10 | measured |
| `target.cc` | runtime | cpp | 9 | measured |
| `tests/move.cc` | tests | cpp | 10 | measured |
| `zero.go` | runtime | go | 13 | oversized |
