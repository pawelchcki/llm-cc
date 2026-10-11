# CI recipe acceptance experiments

The JSON files in `results/` record the exact scorer, model and storage
identities used by each experiment. `rocm-filesystem.json` and `rocm-s3.json`
contain real inference with the installed ROCm scorer. The container experiment
below uses the deterministic native test scorer and a five-byte model fixture.
It verifies ownership and artifact transport without GPU inference.

## Reproduce non-root container artifact transfer

Requirements: Linux x86_64, rootless Podman and a built
`//:llm-cc-compare-fake`. Run from the repository root. Use a new output
directory each time. These commands create two local test images; they do not
push images, change runner settings or publish PR comments.

```sh
bazel --nohome_rc build --noannounce_rc //:llm-cc-compare-fake
acceptance_context=$(mktemp -d /tmp/llmcc-container-context.XXXXXX)
mkdir -p "$acceptance_context/scorer/opt/llm-cc/bin" "$acceptance_context/coordinator"
cp bazel-bin/llm-cc-compare-fake "$acceptance_context/scorer/opt/llm-cc/bin/llm-cc"
cat >"$acceptance_context/scorer/Containerfile" <<'EOF'
FROM docker.io/library/ubuntu:24.04
RUN useradd --uid 10001 --user-group --create-home llm-cc
COPY opt/ /opt/
RUN mkdir /models && printf model >/models/model.gguf && chmod 0444 /models/model.gguf
ENV PATH=/opt/llm-cc/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
USER 10001:10001
EOF
podman build --platform linux/amd64 --tag localhost/llmcc-acceptance-scorer \
  "$acceptance_context/scorer"
acceptance_scorer=$(podman image inspect localhost/llmcc-acceptance-scorer \
  --format '{{index .RepoDigests 0}}')
cp -R tools "$acceptance_context/coordinator/"
cp tools/comparison/recipe/config/rules.example.json "$acceptance_context/coordinator/rules.json"
printf '%s\n' --backend cpu --entropy-reduction host --model-sha256 \
  9372c470eeadd5ecd9c3c74c2b3cb633f8e2f2fad799250a0f70d652b6b825e4 \
  --model-bytes 5 >"$acceptance_context/coordinator/scoring.args"
printf '%s\n' --max-file-bytes 49152 >"$acceptance_context/coordinator/planning.args"
podman build --platform linux/amd64 --file tools/comparison/recipe/images/coordinator.Containerfile \
  --tag localhost/llmcc-acceptance-coordinator \
  --build-arg "SCORER_IMAGE=$acceptance_scorer" \
  --build-arg "LLM_CC_COMMIT=$(git rev-parse HEAD)" "$acceptance_context/coordinator"
acceptance_coordinator=$(podman image inspect localhost/llmcc-acceptance-coordinator \
  --format '{{index .RepoDigests 0}}')
PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=. \
  python3 experiments/ci-recipe-acceptance/container_artifacts.py \
    --scorer "$acceptance_scorer" --coordinator "$acceptance_coordinator" \
    --output "$acceptance_context/artifact-run"
```

The experiment requires the `deterministic-test/entropy-v1` scorer ABI and
image references by digest. It starts 13 fresh containers, all as UID 10001,
with networking disabled. Podman's `keep-id` mapping lets that container user
write artifacts owned by the invoking host user. The four workers run in
isolated directories. Each receives only a preparation ZIP, writes its native
worker artifact, and transfers it in a separate ZIP. Aggregation extracts
worker ZIPs before preparation and verifies that every worker file retained
its exact contents. Both reports must be complete, category totals must match,
and warm preparation/aggregation must schedule zero workers within 60 seconds.

`acceptance.json` contains the image/scorer/model identities, worker hashes
and warm timing. Every container invocation retains a numbered log. Keep the
output directory when investigating failures. The CPU coordinator test suite
can also run in the same image:

```sh
podman run --rm --network=none --env LLM_CC=/opt/llm-cc/bin/llm-cc \
  "$acceptance_coordinator" python3 -m unittest discover \
    -s /opt/llm-cc-comparison/tools/comparison -t /opt/llm-cc-comparison -p 'test_*.py'
```

These local containers do not exercise a hosted artifact service, NVIDIA
runtime injection, the CUDA scorer Containerfile or the real model image.
Those remain separate acceptance checks in
[VALIDATION.md](../../tools/comparison/VALIDATION.md).

## Real CUDA images and model-layer reuse

[`results/cuda-image-reuse.json`](results/cuda-image-reuse.json) records a
separate run of the original model, CUDA scorer and coordinator Containerfiles
with the real 14 GB GGUF. This ran on Bazzite without an NVIDIA device or
organizational credentials; it verifies builds and offline installed identities.
It does not perform GPU inference. The published registry was disposable and
bound to loopback, using TLS with an experiment-local CA. The recorded image
references identify that local registry and are not public downloads.

To repeat it, configure a registry and the required environment documented in
[CI_RECIPE.md](../../tools/comparison/CI_RECIPE.md), using the model, base-image
and Bazelisk pins in the result JSON. Use `CUDA_ARCHS=compute_89:sm_89` for this
experiment. Two clean checkouts reproduce the recorded source revisions:

```sh
git worktree add --detach /tmp/llmcc-cuda-current bcfea9ae2ee262511f5b7ab90da9a8d776e1974d
git worktree add --detach /tmp/llmcc-cuda-base 7bb7671cb850b14116beafbcdf55adddc7817085
export LLM_CC_SOURCE=/tmp/llmcc-cuda-current
export LLM_CC_COMMIT=bcfea9ae2ee262511f5b7ab90da9a8d776e1974d
export LLM_CC_VERSION=0.6.1-dev.77+gbcfea9a
export OUTPUT=/tmp/llmcc-cuda-cold
sh tools/comparison/recipe/images/build.sh
export OUTPUT=/tmp/llmcc-cuda-warm
sh tools/comparison/recipe/images/build.sh
cmp /tmp/llmcc-cuda-cold/images.env /tmp/llmcc-cuda-warm/images.env
cmp /tmp/llmcc-cuda-cold/identity.json /tmp/llmcc-cuda-warm/identity.json
export LLM_CC_SOURCE=/tmp/llmcc-cuda-base
export LLM_CC_COMMIT=7bb7671cb850b14116beafbcdf55adddc7817085
export LLM_CC_VERSION=0.6.1-dev.76+g7bb7671
export OUTPUT=/tmp/llmcc-cuda-rebuild
sh tools/comparison/recipe/images/build.sh
```

The warm log must report three reused images and contain no build or push
steps. Obtain each immutable image's registry manifest with
`skopeo inspect --raw docker://NAME@sha256:DIGEST`: the model has one layer,
and each scorer's first layer must match it exactly. The coordinator's layers
must omit that model layer. Supply your registry CA through the tools' normal
certificate configuration; this experiment retained TLS verification.

Run each scorer and coordinator with `podman run --rm --network=none` and
confirm UID 10001. Read the coordinator's baked `scoring.args`, then run
`llm-cc compare identity --scorer-image SCORER_DIGEST` with those same arguments
in both images; their JSON must match `build.sh`'s identity. Inside each scorer,
verify `/models/model.gguf` with `stat -c %s` and `sha256sum`. Inside each
coordinator, confirm that file is absent. Different source revisions must
produce different scorer image references and scoring fingerprints.

The recorded warm image reuse took **1.845 seconds**, with no builds or pushes.
The cold Bazel build took **769.117 seconds**; the second revision used 1,330
action-cache hits and six new actions in **10.942 seconds**. These Bazel timings
exclude model distribution and other container stages. Both runtime identities
and weight hashes passed offline. The 124 provider tests passed in the real
coordinator with the complete test source and deterministic fixture scorer
mounted read-only; the production image alone omits those fixture assets.
