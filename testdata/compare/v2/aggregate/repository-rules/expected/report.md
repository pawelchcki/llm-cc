## llm-cc comparison

Status: **incomplete**

Scores and ranks use total LM-CC; LM-CC/token is secondary.

- runtime unavailable → unavailable LM-CC (unavailable)
- tests 1.5 → 1.5 LM-CC (+0%)
- tooling 0.0 → 0.0 LM-CC (unavailable)
- repository unavailable → unavailable LM-CC (unavailable)

| Category | LM-CC base | LM-CC head | LM-CC Δ | LM-CC Δ% | LM-CC/token Δ | Tokens Δ | Coverage |
|---|---:|---:|---:|---:|---:|---:|---:|
| runtime | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | 87.5% / 87.5% |
| tests | 1.5 | 1.5 | +0.0 | +0% | 0 | 0 | 100.0% / 100.0% |
| tooling | 0.0 | 0.0 | +0.0 | unavailable | unavailable | 0 | 100.0% / 100.0% |
| repository | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | 88.9% / 88.9% |

Cache: 0 hits, 9 misses.

### Changed files

| Path | Category | LM-CC base | LM-CC head | LM-CC Δ | LM-CC Δ% | Head rank |
|---|---|---:|---:|---:|---:|---:|
| `api.hpp` | runtime | 3.8 | 19.5 | +15.8 | +420% | #1/8 |

### Leading regressions

- `api.hpp`: +15.8 LM-CC (+0.835 LM-CC/token)

<details>
<summary>Top offenders on the merge base (`39be199bcdc7b54f6258e08e4f585fee7eb6e20c`)</summary>

| # | Path | Category | LM-CC | Touched |
|---:|---|---|---:|---|
| 1 | `same.cc` | runtime | 17.2 |  |
| 2 | `src_test.cc` | runtime | 15.0 |  |
| 3 | `odd\u000aname.py` | runtime | 12.8 |  |
| 4 | `copy.cc` | runtime | 10.5 |  |
| 5 | `target.cc` | runtime | 8.2 |  |
| 6 | `zero.go` | runtime | 6.0 |  |
| 7 | `api.hpp` | runtime | 3.8 | yes |
| 8 | `tests/move.cc` | tests | 1.5 |  |

</details>

Rules: repository `.llm-cc/comparison-rules.json`@265aa59

### Raw totals

| Side/category | LM-CC | Tokens | LM-CC/token |
|---|---:|---:|---:|
| base/runtime | unavailable | unavailable | unavailable |
| base/tests | 1.5 | 3 | 0.5 |
| base/tooling | 0.0 | 0 | unavailable |
| head/runtime | unavailable | unavailable | unavailable |
| head/tests | 1.5 | 3 | 0.5 |
| head/tooling | 0.0 | 0 | unavailable |

### Coverage details

- Base: 8/9 supported paths measured; unscored paths: oversized: 1, symlink: 1, unsupported: 1
- Head: 8/9 supported paths measured; unscored paths: oversized: 1, symlink: 1, unsupported: 1

### Changed paths

- `api.hpp` → `api.hpp` (M)

### Base inventory

| Path | Category | Language | Bytes | Measurement |
|---|---|---|---:|---|
| `README.md` | runtime | — | 5 | unsupported |
| `api.hpp` | runtime | cpp | 11 | measured |
| `copy.cc` | runtime | cpp | 7 | measured |
| `large.py` | runtime | python | 120 | oversized |
| `link.cc` | runtime | cpp | 9 | symlink |
| `odd\u000aname.py` | runtime | python | 6 | measured |
| `same.cc` | runtime | cpp | 13 | measured |
| `src_test.cc` | runtime | cpp | 10 | measured |
| `target.cc` | runtime | cpp | 9 | measured |
| `tests/move.cc` | tests | cpp | 10 | measured |
| `zero.go` | runtime | go | 13 | measured |

### Head inventory

| Path | Category | Language | Bytes | Measurement |
|---|---|---|---:|---|
| `README.md` | runtime | — | 5 | unsupported |
| `api.hpp` | runtime | cpp | 14 | measured |
| `copy.cc` | runtime | cpp | 7 | measured |
| `large.py` | runtime | python | 120 | oversized |
| `link.cc` | runtime | cpp | 9 | symlink |
| `odd\u000aname.py` | runtime | python | 6 | measured |
| `same.cc` | runtime | cpp | 13 | measured |
| `src_test.cc` | runtime | cpp | 10 | measured |
| `target.cc` | runtime | cpp | 9 | measured |
| `tests/move.cc` | tests | cpp | 10 | measured |
| `zero.go` | runtime | go | 13 | measured |
