## llm-cc comparison

Status: **complete**

Scores and ranks use total LM-CC; LM-CC/token is secondary.

- runtime 1.0 → 2.0 LM-CC (+100%)
- tests 1.0 → 2.0 LM-CC (+100%)
- tooling 1.0 → 2.0 LM-CC (+100%)
- repository 1.0 → 2.0 LM-CC (+100%)

| Category | LM-CC base | LM-CC head | LM-CC Δ | LM-CC Δ% | LM-CC/token Δ | Tokens Δ | Coverage |
|---|---:|---:|---:|---:|---:|---:|---:|
| runtime | 1.0 | 2.0 | +1.0 | +100% | 1 | 0 | 100.0% / 100.0% |
| tests | 1.0 | 2.0 | +1.0 | +100% | 1 | 0 | 100.0% / 100.0% |
| tooling | 1.0 | 2.0 | +1.0 | +100% | 1 | 0 | 100.0% / 100.0% |
| repository | 1.0 | 2.0 | +1.0 | +100% | 1 | 0 | 100.0% / 100.0% |

Cache: 1 hits, 0 misses.

### Leading regressions

- `a.cc`: +2.5 LM-CC (+1 LM-CC/token)
- `legacy.cc`: +0.5 LM-CC/token
- `empty.cc`: +3.0 LM-CC

### Leading improvements

- `b.cc`: -12.1 LM-CC (-0.25 LM-CC/token)

Rules: host
