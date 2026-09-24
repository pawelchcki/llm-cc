# Changelog

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

