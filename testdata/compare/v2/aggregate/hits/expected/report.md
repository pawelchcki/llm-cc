## llm-cc comparison

Status: **incomplete**

Scores and ranks use total LM-CC; LM-CC/token is secondary.

- runtime 33.8 → unavailable LM-CC (unavailable)
- tests 4.0 → 8.0 LM-CC (+100%)
- tooling 4.0 → 0.0 LM-CC (-100%)
- repository 41.8 → unavailable LM-CC (unavailable)

| Category | LM-CC base | LM-CC head | LM-CC Δ | LM-CC Δ% | LM-CC/token Δ | Tokens Δ | Coverage |
|---|---:|---:|---:|---:|---:|---:|---:|
| runtime | 33.8 | unavailable | unavailable | unavailable | unavailable | unavailable | 100.0% / 85.7% |
| tests | 4.0 | 8.0 | +4.0 | +100% | 0 | 4 | 100.0% / 100.0% |
| tooling | 4.0 | 0.0 | -4.0 | -100% | unavailable | -4 | 100.0% / 100.0% |
| repository | 41.8 | unavailable | unavailable | unavailable | unavailable | unavailable | 100.0% / 88.9% |

Cache: 5 hits, 4 misses.

### Changed files

| Path | Category | LM-CC base | LM-CC head | LM-CC Δ | LM-CC Δ% | Head rank |
|---|---|---:|---:|---:|---:|---:|
| `same.cc` | runtime | 4.0 | 7.2 | +3.2 | +81.2% | #3/8 |
| `tests/move.cc` | tests | 4.0 | 4.0 | +0.0 | +0% | #7/8 |
| `tools/gen.py` | tooling | 4.0 | — | — | — | — |
| `zero.go` | runtime | — | 4.0 | — | — | #8/8 |

### Leading regressions

- `zero.go`: +4.0 LM-CC
- `same.cc`: +3.2 LM-CC (+0.45 LM-CC/token)

### Leading improvements

- `tools/gen.py`: -4.0 LM-CC

<details>
<summary>Top offenders on the merge base (`1d43f119f327a404e8f9633434f83e55995f2369`)</summary>

| # | Path | Category | LM-CC | Touched |
|---:|---|---|---:|---|
| 1 | `api.hpp` | runtime | 7.2 |  |
| 2 | `odd\u000aname.py` | runtime | 7.2 |  |
| 3 | `target.cc` | runtime | 7.2 |  |
| 4 | `copy.cc` | runtime | 4.0 |  |
| 5 | `move.cc` | runtime | 4.0 | yes |
| 6 | `same.cc` | runtime | 4.0 | yes |
| 7 | `src_test.cc` | tests | 4.0 |  |
| 8 | `tools/gen.py` | tooling | 4.0 | yes |

</details>

Baseline ranking: <https://ci.example/o/r/baseline.md>

Rules: host

### Raw totals

| Side/category | LM-CC | Tokens | LM-CC/token |
|---|---:|---:|---:|
| base/runtime | 33.8 | 27 | 1.25 |
| base/tests | 4.0 | 4 | 1 |
| base/tooling | 4.0 | 4 | 1 |
| head/runtime | unavailable | unavailable | unavailable |
| head/tests | 8.0 | 8 | 1 |
| head/tooling | 0.0 | 0 | unavailable |

### Coverage details

- Base: 8/8 supported paths measured; unscored paths: symlink: 1, unsupported: 1
- Head: 8/9 supported paths measured; unscored paths: oversized: 1, symlink: 1, unsupported: 1

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
| `api.hpp` | runtime | cpp | 11 | measured |
| `copy.cc` | runtime | cpp | 7 | measured |
| `link.cc` | runtime | cpp | 9 | symlink |
| `move.cc` | runtime | cpp | 10 | measured |
| `odd\u000aname.py` | runtime | python | 6 | measured |
| `same.cc` | runtime | cpp | 7 | measured |
| `src_test.cc` | tests | cpp | 10 | measured |
| `target.cc` | runtime | cpp | 9 | measured |
| `tools/gen.py` | tooling | python | 16 | measured |

### Head inventory

| Path | Category | Language | Bytes | Measurement |
|---|---|---|---:|---|
| `README.md` | runtime | — | 5 | unsupported |
| `api.hpp` | runtime | cpp | 11 | measured |
| `copy.cc` | runtime | cpp | 7 | measured |
| `large.py` | runtime | python | 120 | oversized |
| `link.cc` | runtime | cpp | 9 | symlink |
| `odd\u000aname.py` | runtime | python | 6 | measured |
| `same.cc` | runtime | cpp | 13 | measured |
| `src_test.cc` | tests | cpp | 10 | measured |
| `target.cc` | runtime | cpp | 9 | measured |
| `tests/move.cc` | tests | cpp | 10 | measured |
| `zero.go` | runtime | go | 13 | measured |
