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

### Changed files

| Path | Category | LM-CC base | LM-CC head | LM-CC Δ | LM-CC Δ% | Head rank |
|---|---|---:|---:|---:|---:|---:|
| ```src/@team/[file]`name``x`~~$y$\|pipe\|https://example.com/a.cc``` | runtime | 1.0 | 4.0 | +3.0 | +100% | #3/130 |

### Leading regressions

- ```src/@team/[file]`name``x`~~$y$|pipe|https://example.com/a.cc```: +1 LM-CC/token

Rules: host
