# CI recipe templates

Copyable templates for the advisory PR complexity pipeline described in
[CI_RECIPE.md](../CI_RECIPE.md). Replace every `OWNER/REPO`, placeholder
digest and `YOUR_*` value before use.

| File | Purpose |
|---|---|
| [gitlab/.gitlab-ci.yml](gitlab/.gitlab-ci.yml) | GitLab parent pipeline: images (on demand), prepare, generated child pipeline, serialized publisher |
| [github/comparison.yml](github/comparison.yml) | GitHub Actions workflow: prepare, dynamic GPU matrix, aggregate, publisher |
| [images/model.Containerfile](images/model.Containerfile) | Content-keyed model image: verified GGUF weights on `scratch` |
| [images/scorer.Containerfile](images/scorer.Containerfile) | GPU image built on the model image by digest: CUDA scorer, worker, non-root user |
| [images/coordinator.Containerfile](images/coordinator.Containerfile) | CPU image: Git, the comparison package, boto3, the scoring profile and default rules |
| [images/build.sh](images/build.sh) | Builds or verifies and reuses all three images; writes `images.env` and `profile.json` |
| [config/ci.example.json](config/ci.example.json) | Optional `ci gitlab-child` settings: runner tags, worker capacity, timeouts |
| [config/rules.example.json](config/rules.example.json) | Default classification rules baked into the coordinator |
| [config/store-options.example.json](config/store-options.example.json) | Optional S3 client options (never credentials) |

Per-repository rules belong in `.llm-cc/rules.json` on the target
branch; see [README.md](../README.md#per-repository-classification-rules).
