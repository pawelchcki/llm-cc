# Changelog

## Unreleased

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

