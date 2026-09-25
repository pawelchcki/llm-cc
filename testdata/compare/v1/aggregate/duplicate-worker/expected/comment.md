## llm-cc comparison

Status: **failed**

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

Errors:
- `unexpected or duplicate worker 0`

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
