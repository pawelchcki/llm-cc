## llm-cc comparison

Status: **incomplete**

Scores and ranks use total LM-CC; LM-CC/token is secondary.

- runtime 47.2 → unavailable LM-CC (unavailable)
- tests 15.0 → 16.5 LM-CC (+10%)
- tooling 19.5 → 0.0 LM-CC (-100%)
- repository 81.8 → unavailable LM-CC (unavailable)

| Category | LM-CC base | LM-CC head | LM-CC Δ | LM-CC Δ% | LM-CC/token Δ | Tokens Δ | Coverage |
|---|---:|---:|---:|---:|---:|---:|---:|
| runtime | 47.2 | unavailable | unavailable | unavailable | unavailable | unavailable | 100.0% / 85.7% |
| tests | 15.0 | 16.5 | +1.5 | +10% | -0.291667 | 3 | 100.0% / 100.0% |
| tooling | 19.5 | 0.0 | -19.5 | -100% | unavailable | -11 | 100.0% / 100.0% |
| repository | 81.8 | unavailable | unavailable | unavailable | unavailable | unavailable | 100.0% / 88.9% |

Cache: 0 hits, 9 misses.

### Changed files

| Path | Category | LM-CC base | LM-CC head | LM-CC Δ | LM-CC Δ% | Head rank |
|---|---|---:|---:|---:|---:|---:|
| `same.cc` | runtime | 10.5 | 17.2 | +6.8 | +64.3% | #1/8 |
| `tests/move.cc` | tests | 1.5 | 1.5 | +0.0 | +0% | #8/8 |
| `tools/gen.py` | tooling | 19.5 | — | — | — | — |
| `zero.go` | runtime | — | 1.6 | — | — | #7/8 |

### Leading regressions

- `same.cc`: +6.8 LM-CC (+0.225 LM-CC/token)
- `zero.go`: +1.6 LM-CC

### Leading improvements

- `tools/gen.py`: -19.5 LM-CC

<details>
<summary>Top offenders on the merge base (`1d43f119f327a404e8f9633434f83e55995f2369`)</summary>

| # | Path | Category | LM-CC | Touched |
|---:|---|---|---:|---|
| 1 | `tools/gen.py` | tooling | 19.5 | yes |
| 2 | `src_test.cc` | tests | 15.0 |  |
| 3 | `odd\u000aname.py` | runtime | 12.8 |  |
| 4 | `copy.cc` | runtime | 10.5 |  |
| 5 | `same.cc` | runtime | 10.5 | yes |
| 6 | `target.cc` | runtime | 8.2 |  |
| 7 | `api.hpp` | runtime | 3.8 |  |
| 8 | `move.cc` | runtime | 1.5 | yes |

</details>

Rules: host

### Raw totals

| Side/category | LM-CC | Tokens | LM-CC/token |
|---|---:|---:|---:|
| base/runtime | 47.2 | 35 | 1.35 |
| base/tests | 15.0 | 9 | 1.66667 |
| base/tooling | 19.5 | 11 | 1.77273 |
| head/runtime | unavailable | unavailable | unavailable |
| head/tests | 16.5 | 12 | 1.375 |
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
