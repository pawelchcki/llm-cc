## llm-cc comparison

Status: **complete**

Scores and ranks use total LM-CC; LM-CC/token is secondary.

- runtime 2755.6 → 2373.1 LM-CC (-13.9%)
- tests 73.5 → 183.0 LM-CC (+149%)
- tooling 1.5 → 1.5 LM-CC (+0%)
- repository 2830.6 → 2557.6 LM-CC (-9.64%)

| Category | LM-CC base | LM-CC head | LM-CC Δ | LM-CC Δ% | LM-CC/token Δ | Tokens Δ | Coverage |
|---|---:|---:|---:|---:|---:|---:|---:|
| runtime | 2755.6 | 2373.1 | -382.5 | -13.9% | 0.00701671 | -184 | 100.0% / 100.0% |
| tests | 73.5 | 183.0 | +109.5 | +149% | 0.027907 | 51 | 100.0% / 100.0% |
| tooling | 1.5 | 1.5 | +0.0 | +0% | 0 | 0 | 100.0% / 100.0% |
| repository | 2830.6 | 2557.6 | -273.0 | -9.64% | 0.00712459 | -133 | 100.0% / 100.0% |

Cache: 0 hits, 61 misses.

### Changed files

| Path | Category | LM-CC base | LM-CC head | LM-CC Δ | LM-CC Δ% | Head rank |
|---|---|---:|---:|---:|---:|---:|
| `src/gen08.c` | runtime | 114.0 | 3.8 | -110.2 | -96.7% | #36/37 |
| `src/gen16.c` | runtime | 28.5 | 127.5 | +99.0 | +347% | #5/37 |
| `src/pipe\|name.cc` | runtime | 19.5 | 116.2 | +96.8 | +496% | #7/37 |
| ```src/tick`name``x.cc``` | runtime | 10.5 | 102.8 | +92.2 | +879% | #11/37 |
| `src/gen04.c` | runtime | 120.8 | 33.0 | -87.8 | -72.7% | #29/37 |
| `src/gen00.c` | runtime | 96.0 | 17.2 | -78.8 | -82% | #32/37 |
| `src/bidi\u2066x\u2069.go` | runtime | 37.5 | 109.5 | +72.0 | +192% | #9/37 |
| `src/gen14.c` | runtime | 125.2 | 53.2 | -72.0 | -57.5% | #22/37 |
| `src/gen02.c` | runtime | 30.8 | 80.2 | +49.5 | +161% | #16/37 |
| `src/gen20.c` | runtime | 82.5 | 132.0 | +49.5 | +60% | #3/37 |
| `src/gen06.c` | runtime | 39.8 | 66.8 | +27.0 | +67.9% | #19/37 |
| `tests/case_test.cc` | tests | 73.5 | 46.5 | -27.0 | -36.7% | #24/37 |
| `src/gen28.c` | runtime | 64.5 | 42.0 | -22.5 | -34.9% | #26/37 |
| `src/zażółć.py` | runtime | 26.2 | 44.2 | +18.0 | +68.6% | #25/37 |
| `src/gen24.c` | runtime | 75.8 | 91.5 | +15.8 | +20.8% | #14/37 |
| `src/gen18.c` | runtime | 8.2 | 15.0 | +6.8 | +81.8% | #33/37 |
| `src/gen22.c` | runtime | 48.8 | 55.5 | +6.8 | +13.8% | #21/37 |
| `src/@team/[file]<tag>\u202e.cc` | runtime | 118.5 | — | — | — | — |
| `src/added.cc` | runtime | — | 35.2 | — | — | #28/37 |
| `src/gen05.c` | runtime | 62.2 | — | — | — | — |
| `src/gen10.c` | runtime | 69.0 | — | — | — | — |
| `src/gen12.c` | runtime | 93.8 | — | — | — | — |
| `src/gen19.c` | runtime | 87.0 | — | — | — | — |
| `src/gen26.c` | runtime | 89.2 | — | — | — | — |
| `src/star*under_score~.cs` | runtime | 60.0 | — | — | — | — |

_1 more changed files are listed in the full report._

### Leading regressions

- `tests/gen10_test.c`: +136.5 LM-CC
- `src/gen16.c`: +99.0 LM-CC (+0.261 LM-CC/token)
- `src/pipe|name.cc`: +96.8 LM-CC (+0.38 LM-CC/token)
- ```src/tick`name``x.cc```: +92.2 LM-CC (+0.641 LM-CC/token)
- `src/bidi\u2066x\u2069.go`: +72.0 LM-CC (+0.173 LM-CC/token)
- `src/gen02.c`: +49.5 LM-CC (+0.19 LM-CC/token)
- `src/gen20.c`: +49.5 LM-CC (+0.0485 LM-CC/token)
- `src/added.cc`: +35.2 LM-CC
- `src/gen06.c`: +27.0 LM-CC (+0.0984 LM-CC/token)
- `src/zażółć.py`: +18.0 LM-CC (+0.136 LM-CC/token)

### Leading improvements

- `src/@team/[file]<tag>\u202e.cc`: -118.5 LM-CC
- `src/gen08.c`: -110.2 LM-CC (-1.21 LM-CC/token)
- `src/gen12.c`: -93.8 LM-CC
- `src/gen26.c`: -89.2 LM-CC
- `src/gen04.c`: -87.8 LM-CC (-0.215 LM-CC/token)
- `src/gen19.c`: -87.0 LM-CC
- `src/gen00.c`: -78.8 LM-CC (-0.408 LM-CC/token)
- `src/gen14.c`: -72.0 LM-CC (-0.111 LM-CC/token)
- `src/gen10.c`: -69.0 LM-CC
- `src/gen05.c`: -62.2 LM-CC

<details>
<summary>Top offenders on the merge base (`54f30096430571033eb0bd6d6ef625c21b2adac7`)</summary>

| # | Path | Category | LM-CC | Touched |
|---:|---|---|---:|---|
| 1 | `src/gen23.c` | runtime | 134.2 |  |
| 2 | `src/gen15.c` | runtime | 129.8 |  |
| 3 | `src/gen14.c` | runtime | 125.2 | yes |
| 4 | `src/emoji-😀.js` | runtime | 123.0 |  |
| 5 | `src/gen04.c` | runtime | 120.8 | yes |
| 6 | `src/@team/[file]<tag>\u202e.cc` | runtime | 118.5 | yes |
| 7 | `src/gen08.c` | runtime | 114.0 | yes |
| 8 | `src/gen11.c` | runtime | 111.8 |  |
| 9 | `src/back\slash\|pipe.cc` | runtime | 105.0 |  |
| 10 | `src/gen29.c` | runtime | 100.5 |  |

</details>

Rules: host
