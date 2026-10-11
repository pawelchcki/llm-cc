# Changelog

## [0.8.0](https://github.com/pawelchcki/llm-cc/compare/v0.7.1...v0.8.0) (2026-10-08)


### Features

* distribute prebuilt binaries through npm and PyPI ([#82](https://github.com/pawelchcki/llm-cc/issues/82)) ([d2cc08e](https://github.com/pawelchcki/llm-cc/commit/d2cc08e3be9325cdd98bfe7a67cdd5348d083017))

## [0.7.1](https://github.com/pawelchcki/llm-cc/compare/v0.7.0...v0.7.1) (2026-10-07)


### Bug Fixes

* **test:** close TLS fixtures gracefully on native TLS backends ([#80](https://github.com/pawelchcki/llm-cc/issues/80)) ([ab0ea53](https://github.com/pawelchcki/llm-cc/commit/ab0ea53d643b64f543121d0fa2c91feef4ebeacc))

## [0.7.0](https://github.com/pawelchcki/llm-cc/compare/v0.6.1...v0.7.0) (2026-10-07)


### Features

* **comparison:** route Bazzite jobs by BuildBuddy custom resources ([#77](https://github.com/pawelchcki/llm-cc/issues/77)) ([81b5978](https://github.com/pawelchcki/llm-cc/commit/81b5978fc37fada92f8c8af79146dc6c41551a0e))

### Bug Fixes

* show download phases and stalled transfer progress ([#79](https://github.com/pawelchcki/llm-cc/issues/79)) ([a8fff1f](https://github.com/pawelchcki/llm-cc/commit/a8fff1f26e20282d3d6c30aa0da913c44a35223d))
* **comparison:** pin image platforms and verify non-root artifacts ([#76](https://github.com/pawelchcki/llm-cc/issues/76)) ([186c904](https://github.com/pawelchcki/llm-cc/commit/186c9045a0a8f7af809bf06b9dbcb2094efb3e0b))
* **comparison:** enforce CI worker limits and add live acceptance checks ([#74](https://github.com/pawelchcki/llm-cc/issues/74)) ([7bb7671](https://github.com/pawelchcki/llm-cc/commit/7bb7671cb850b14116beafbcdf55adddc7817085))

## [0.6.1](https://github.com/pawelchcki/llm-cc/compare/v0.6.0...v0.6.1) (2026-10-04)


### Bug Fixes

* **metal:** defer shader compilation during device preflight ([#72](https://github.com/pawelchcki/llm-cc/issues/72)) ([7d274b3](https://github.com/pawelchcki/llm-cc/commit/7d274b3a405cdac97c967719e804820f1757d4ac))

## [0.6.0](https://github.com/pawelchcki/llm-cc/compare/v0.5.2...v0.6.0) (2026-10-03)


### Features

* add native readiness checks and GPU selection ([#67](https://github.com/pawelchcki/llm-cc/issues/67)) ([962cfa1](https://github.com/pawelchcki/llm-cc/commit/962cfa1acb82c4257167665de27b79112e0046d4))

## [0.5.2](https://github.com/pawelchcki/llm-cc/compare/v0.5.1...v0.5.2) (2026-10-01)


### Bug Fixes

* verify draft release assets by release ID ([#68](https://github.com/pawelchcki/llm-cc/issues/68)) ([407e1f1](https://github.com/pawelchcki/llm-cc/commit/407e1f1be0dc590252bf283645fb9363c075ed34))

## [0.5.1](https://github.com/pawelchcki/llm-cc/compare/v0.5.0...v0.5.1) (2026-09-30)


### Bug Fixes

* publish releases only after all artifacts are uploaded ([#64](https://github.com/pawelchcki/llm-cc/issues/64)) ([16341c4](https://github.com/pawelchcki/llm-cc/commit/16341c4b49b36da81eb7b4727bea62246fa5f7a1))

## [0.5.0](https://github.com/pawelchcki/llm-cc/compare/v0.4.0...v0.5.0) (2026-09-28)


### Features

* **comparison:** collapse the PR comment behind two LM-CC delta lines ([#62](https://github.com/pawelchcki/llm-cc/issues/62)) ([4f15d77](https://github.com/pawelchcki/llm-cc/commit/4f15d77f8452a2481e75936a089bdaa5bb5fc077))

## [0.4.0](https://github.com/pawelchcki/llm-cc/compare/v0.3.0...v0.4.0) (2026-09-28)


### Features

* **ci:** publish artifacts and clean merged PRs with Runnerless ([#61](https://github.com/pawelchcki/llm-cc/issues/61)) ([1185bdf](https://github.com/pawelchcki/llm-cc/commit/1185bdf67ef70bf3d13f84841eae85814feae20e))
* **comparison:** score files of any size the analyzer accepts ([#59](https://github.com/pawelchcki/llm-cc/issues/59)) ([2ea658a](https://github.com/pawelchcki/llm-cc/commit/2ea658aada6080322e9fef033200e3d12e434ef5))
* **comparison:** move the comparison pipeline into llm-cc compare ([#56](https://github.com/pawelchcki/llm-cc/issues/56)) ([e1740fc](https://github.com/pawelchcki/llm-cc/commit/e1740fc5b722b4bebb1693c0ef549f8086283b1f))
* **comparison:** render reports and read stores in llm-cc ([#55](https://github.com/pawelchcki/llm-cc/issues/55)) ([ba0e600](https://github.com/pawelchcki/llm-cc/commit/ba0e600eec537b3b994d60f8354e0d220f621dea))
* one rules engine and shell-free Git for llm-cc ([#54](https://github.com/pawelchcki/llm-cc/issues/54)) ([68bc7f3](https://github.com/pawelchcki/llm-cc/commit/68bc7f3d532f29e6afe0f37465f7558819d45058))

### Bug Fixes

* build and test the release on Windows and macOS ([#58](https://github.com/pawelchcki/llm-cc/issues/58)) ([4df0cb7](https://github.com/pawelchcki/llm-cc/commit/4df0cb7ca2accd4c0730849c889c00cc9f2817d7))
* **comparison:** render comments ci-toolkit can publish ([#60](https://github.com/pawelchcki/llm-cc/issues/60)) ([5a17ccf](https://github.com/pawelchcki/llm-cc/commit/5a17ccfa1eae991c2853dfcc6b2e63ac13d09883))
* **comparison:** use total LM-CC consistently ([#53](https://github.com/pawelchcki/llm-cc/issues/53)) ([2ade5c0](https://github.com/pawelchcki/llm-cc/commit/2ade5c0e1067f6ae5064d72226270106850828ec))

## [0.3.0](https://github.com/pawelchcki/llm-cc/compare/v0.2.0...v0.3.0) (2026-09-24)


### Features

* add a curl/wget installer shipped with every release ([#49](https://github.com/pawelchcki/llm-cc/issues/49)) ([76d4ffe](https://github.com/pawelchcki/llm-cc/commit/76d4ffef538f9f331c4367f2454321f0a550cef0))
* **comparison:** add a reproducible CI recipe for cached GPU scoring ([#47](https://github.com/pawelchcki/llm-cc/issues/47)) ([10ee9f4](https://github.com/pawelchcki/llm-cc/commit/10ee9f4268bacf28ea51a4acf525f2c6081209df))
* **comparison:** generalize profiles, complete provenance, and speed up cache reads ([#46](https://github.com/pawelchcki/llm-cc/issues/46)) ([abaf02f](https://github.com/pawelchcki/llm-cc/commit/abaf02f0f9113415b5f68bc7146dbba2fce7fbd6))
* report paper-scale LM-CC with per-model calibrated tau ([#45](https://github.com/pawelchcki/llm-cc/issues/45)) ([526b40d](https://github.com/pawelchcki/llm-cc/commit/526b40d2a4d9a88c2aa87d494de0f44bd3c0a4b5))
* rank comparison files and support consumer repositories ([#43](https://github.com/pawelchcki/llm-cc/issues/43)) ([6011717](https://github.com/pawelchcki/llm-cc/commit/60117176d67b41ce23e96ac5a32518a3d3bff1df))
* score large files with overlapping context windows ([#42](https://github.com/pawelchcki/llm-cc/issues/42)) ([e72466c](https://github.com/pawelchcki/llm-cc/commit/e72466ca4d0236a68bb13e4fb75050fad71266da))
* add cached complexity comparisons and Radeon dogfooding ([#39](https://github.com/pawelchcki/llm-cc/issues/39)) ([08d0f79](https://github.com/pawelchcki/llm-cc/commit/08d0f79aa59807a7ba48543703eed0ba3bb9341a))

### Bug Fixes

* scale large-model inference and hashing ([#36](https://github.com/pawelchcki/llm-cc/issues/36)) ([4646123](https://github.com/pawelchcki/llm-cc/commit/4646123b274005c587dfeb614f17ddf5fd36aef6))

### Performance Improvements

* cut CI time by reusing caches, isolating stamps, and hashing faster ([#48](https://github.com/pawelchcki/llm-cc/issues/48)) ([2b73bf6](https://github.com/pawelchcki/llm-cc/commit/2b73bf6e3ab4396e277454022a1648659bd3f820))

## Unreleased

### Added

* add a reproducible CI recipe for cached GPU comparisons
  ([tools/comparison/CI_RECIPE.md](tools/comparison/CI_RECIPE.md)): `discover`,
  `store-report`, `publish` and `ci` comparison commands, GitLab parent/child
  and GitHub Actions templates, and Containerfiles for the model, scorer and
  coordinator images ([#38](https://github.com/pawelchcki/llm-cc/issues/38))
* publish one pull-request comment per PR natively, ordered by pipeline with
  conditional store writes, re-checking the PR before every update and still
  reporting failures when no report was produced

### Changed

* report raw LM-CC, the paper's metric, as the default headline (`--score raw`);
  per-token scoring stays available with `--score lmcc`, and totals add
  `mean_llm_cc_per_file`
* calibrate the default entropy threshold per registered model to the entropy
  percentile of the paper's CodeLlama-7b threshold (`tau_source` in the
  configuration event); re-baseline existing scores
* accept the SentencePiece dummy-prefix space on the first token, so
  CodeLlama-style models can be analyzed
* never let the file's first code token open an entropy boundary, matching the
  reference implementation; `analysis_version` is now 3
* download registered models from the Hugging Face revisions their default
  tau was calibrated on, and decline a calibrated tau when a cached file's
  digest does not match the registered model, falling back to the paper
  threshold (`tau_source` reports `model-digest-mismatch`)
* publish entropy-cache entries for SentencePiece models such as CodeLlama,
  whose dummy-prefix token made every entry fail cache validation so no run
  could ever reach a cache hit
* register `codellama-7b-q8_0`, the paper's reference model

## [0.2.0](https://github.com/pawelchcki/llm-cc/compare/v0.1.0...v0.2.0) (2026-09-07)


### Features

* publish complete platform releases and automatically install matching packages ([#24](https://github.com/pawelchcki/llm-cc/issues/24)) ([233c0e3](https://github.com/pawelchcki/llm-cc/commit/233c0e3c1cdd83335f7b5b5b092a6c3469779600))
* make GPU execution predictable and improve CLI progress ([#18](https://github.com/pawelchcki/llm-cc/issues/18)) ([cd89966](https://github.com/pawelchcki/llm-cc/commit/cd899662be0b5ae366316e75a761905347b6a7fb))
* add shared content-addressed entropy cache ([#16](https://github.com/pawelchcki/llm-cc/issues/16)) ([ae84218](https://github.com/pawelchcki/llm-cc/commit/ae84218678c54e0d351c605a4ff23b14f1234975))
* native Windows/MSVC support with Unicode-safe paths ([#12](https://github.com/pawelchcki/llm-cc/issues/12)) ([7dd469f](https://github.com/pawelchcki/llm-cc/commit/7dd469f26581b8237c05e2f16f9285ef841d8b5a))
* CPU-first builds with separately published GPU backends ([#11](https://github.com/pawelchcki/llm-cc/issues/11)) ([31a5cb9](https://github.com/pawelchcki/llm-cc/commit/31a5cb94f3fd530abde74f350b4ebd7d05e19dc5))

