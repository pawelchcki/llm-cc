#!/bin/sh
# Build or reuse the recipe's model, scorer and coordinator images.
#
# Content-derived tags are lookup hints only. A tag found in the registry is
# reused after its digest passes signature and source-identity checks; a fresh
# build uses the digest its push reports, never a tag resolved afterwards.
# Writes $OUTPUT/images.env with immutable name@sha256 references and
# $OUTPUT/profile.json, the scoring profile baked into the coordinator.
#
# Needs podman, skopeo and sha256sum; cosign when signatures are configured.
# Run it from the llm-cc checkout at LLM_CC_COMMIT.
set -eu

: "${REGISTRY:?repository prefix, for example registry.example.com/llm-cc}"
: "${LLM_CC_COMMIT:?full llm-cc commit to build}"
: "${LLM_CC_VERSION:?version that commit carries}"
: "${MODEL_URL:?immutable GGUF download URL}"
: "${MODEL_SHA256:?GGUF SHA-256}"
: "${MODEL_BYTES:?GGUF size in bytes}"
: "${BAZELISK_URL:?bazelisk-linux-amd64 download URL}"
: "${BAZELISK_SHA256:?bazelisk SHA-256}"
CUDA_ARCHS="${CUDA_ARCHS:-compute_86:sm_86}"
# The pinned A10-class CUDA scoring contract; every setting is explicit.
SCORING="${SCORING:---backend cuda --gpu-layers -1 --context 131072 --batch-size 256 --flash-attn on --kv-cache-type q8_0 --kv-offload on --entropy-reduction device --hierarchy structural --tau 0.67 --alpha 0.8 --score-mode lmcc --max-file-bytes 49152}"
ENGINE="${ENGINE:-podman}"
OUTPUT="${OUTPUT:-comparison}"
recipe="$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)"
LLM_CC_SOURCE="$(CDPATH='' cd -- "${LLM_CC_SOURCE:-$recipe/../../..}" && pwd -P)"
mkdir -p "$OUTPUT"
OUTPUT="$(CDPATH='' cd -- "$OUTPUT" && pwd -P)"

# The images must describe exactly the pinned, unmodified source tree. Our own
# outputs are not source: the default OUTPUT lies inside the checkout, and a
# rerun must still get past this check to verify and reuse its images.
case "$OUTPUT" in
  "$LLM_CC_SOURCE")
    echo "OUTPUT must not be the llm-cc checkout itself" >&2
    exit 1
    ;;
  "$LLM_CC_SOURCE"/*) outputs=":(exclude,literal)${OUTPUT#"$LLM_CC_SOURCE"/}" ;;
  *) outputs= ;;
esac
# Assigned first so that a failing `git status` stops the script (set -e)
# instead of reading as a clean tree.
changes="$(git -C "$LLM_CC_SOURCE" status --porcelain -- . ${outputs:+"$outputs"})"
if [ "$(git -C "$LLM_CC_SOURCE" rev-parse HEAD)" != "$LLM_CC_COMMIT" ] || [ -n "$changes" ]; then
  echo "$LLM_CC_SOURCE is not a clean checkout of $LLM_CC_COMMIT" >&2
  exit 1
fi

key() { sha256sum | cut -c1-64; }

verify_signature() {
  if [ -n "${COSIGN_PUBLIC_KEY:-}" ]; then
    cosign verify --key "$COSIGN_PUBLIC_KEY" "$1" >/dev/null
  elif [ "${TRUST_REGISTRY:-0}" = 1 ]; then
    echo "trusting $1 because only this CI can push to the registry" >&2
  else
    echo "not reusing unverified $1; set COSIGN_PUBLIC_KEY or TRUST_REGISTRY=1" >&2
    return 1
  fi
}

# existing NAME TAG [LABEL VALUE]...: the digest reference of NAME:TAG when it
# is signed and each LABEL records its VALUE, the producer's source identity.
existing() {
  name=$1 tag=$2
  shift 2
  digest="$(skopeo inspect --format '{{.Digest}}' "docker://$name:$tag" 2>/dev/null)" ||
    return 1
  while [ "$#" -gt 0 ]; do
    label="$(skopeo inspect --format "{{index .Labels \"$1\"}}" "docker://$name@$digest")"
    if [ "$label" != "$2" ]; then
      echo "not reusing $name@$digest: $1 is '$label', expected '$2'" >&2
      return 1
    fi
    shift 2
  done
  verify_signature "$name@$digest" || return 1
  echo "reusing $name:$tag" >&2
  echo "$name@$digest"
}

# publish NAME TAG CONTEXT CONTAINERFILE [build options]: build, push, sign.
publish() {
  name=$1 tag=$2 context=$3 file=$4
  shift 4
  "$ENGINE" build --file "$file" --tag "$name:$tag" "$@" "$context" >&2
  "$ENGINE" push --digestfile "$OUTPUT/.digest" "$name:$tag" >&2
  reference="$name@$(cat "$OUTPUT/.digest")"
  if [ -n "${COSIGN_PRIVATE_KEY:-}" ]; then
    cosign sign --yes --key "$COSIGN_PRIVATE_KEY" "$reference" >&2
  fi
  echo "$reference"
}

# 1. Model: keyed by the weights themselves. The fetch stage never reaches the
# image, so FETCH_IMAGE pins the download tooling without changing the key.
model_name="$REGISTRY/model"
set -- --build-arg "MODEL_URL=$MODEL_URL" \
  --build-arg "MODEL_SHA256=$MODEL_SHA256" \
  --build-arg "MODEL_BYTES=$MODEL_BYTES"
if [ -n "${FETCH_IMAGE:-}" ]; then set -- "$@" --build-arg "FETCH_IMAGE=$FETCH_IMAGE"; fi
# The size goes into the profile, so a reused image must confirm it just as a
# fresh build's size check would.
model="$(existing "$model_name" "$MODEL_SHA256" io.llm-cc.model.sha256 "$MODEL_SHA256" \
  io.llm-cc.model.bytes "$MODEL_BYTES")" ||
  model="$(publish "$model_name" "$MODEL_SHA256" "$recipe/images" \
    "$recipe/images/model.Containerfile" "$@")"

# 2. Scorer: keyed by every build input, including the model digest.
scorer_name="$REGISTRY/scorer"
scorer_tag="$(
  printf '%s\n' "$LLM_CC_COMMIT" "$LLM_CC_VERSION" "$CUDA_ARCHS" "$model" \
    "${BUILDER_IMAGE:-}" "${RUNTIME_IMAGE:-}" "$BAZELISK_URL" "$BAZELISK_SHA256" |
    cat - "$recipe/images/scorer.Containerfile" | key
)"
set -- --build-arg "MODEL_IMAGE=$model" \
  --build-arg "LLM_CC_COMMIT=$LLM_CC_COMMIT" \
  --build-arg "LLM_CC_VERSION=$LLM_CC_VERSION" \
  --build-arg "CUDA_ARCHS=$CUDA_ARCHS" \
  --build-arg "BAZELISK_URL=$BAZELISK_URL" \
  --build-arg "BAZELISK_SHA256=$BAZELISK_SHA256"
if [ -n "${BUILDER_IMAGE:-}" ]; then set -- "$@" --build-arg "BUILDER_IMAGE=$BUILDER_IMAGE"; fi
if [ -n "${RUNTIME_IMAGE:-}" ]; then set -- "$@" --build-arg "RUNTIME_IMAGE=$RUNTIME_IMAGE"; fi
# Remote-cache credentials reach Bazel only as a build secret.
if [ -n "${BAZELRC:-}" ]; then set -- "$@" --secret "id=bazelrc,src=$BAZELRC"; fi
scorer="$(existing "$scorer_name" "$scorer_tag" org.opencontainers.image.revision "$LLM_CC_COMMIT")" ||
  scorer="$(publish "$scorer_name" "$scorer_tag" "$LLM_CC_SOURCE" \
    "$recipe/images/scorer.Containerfile" "$@")"

# 3. Coordinator: the comparison code plus the profile that names the scorer.
coordinator_name="$REGISTRY/coordinator"
coordinator_tag="$(
  printf '%s\n' "$LLM_CC_COMMIT" "$scorer" "$MODEL_URL" "$SCORING" "${PYTHON_IMAGE:-}" |
    cat - "$recipe/images/coordinator.Containerfile" "$recipe/config/rules.example.json" |
    key
)"
if ! coordinator="$(existing "$coordinator_name" "$coordinator_tag" \
  org.opencontainers.image.revision "$LLM_CC_COMMIT")"; then
  context="$OUTPUT/coordinator-context"
  rm -rf "$context"
  mkdir -p "$context/tools/comparison"
  cp "$LLM_CC_SOURCE/tools/__init__.py" "$context/tools/"
  cp "$LLM_CC_SOURCE"/tools/comparison/*.py "$context/tools/comparison/"
  cp "$recipe/config/rules.example.json" "$context/rules.json"
  # The profile can only name the scorer once its push reported a digest.
  # Generation runs the installed scorer offline; it needs no GPU or network.
  # shellcheck disable=SC2086 # SCORING is deliberately a list of arguments.
  "$ENGINE" run --rm --network=none "$scorer" \
    python3 -m tools.comparison.profile generate --installed-root /opt/llm-cc \
    --execution-image "$scorer" --model-sha256 "$MODEL_SHA256" \
    --model-bytes "$MODEL_BYTES" --model-url "$MODEL_URL" $SCORING \
    --output /dev/stdout >"$context/profile.json"
  set -- --build-arg "LLM_CC_COMMIT=$LLM_CC_COMMIT"
  if [ -n "${PYTHON_IMAGE:-}" ]; then set -- "$@" --build-arg "PYTHON_IMAGE=$PYTHON_IMAGE"; fi
  coordinator="$(publish "$coordinator_name" "$coordinator_tag" "$context" \
    "$recipe/images/coordinator.Containerfile" "$@")"
fi
"$ENGINE" run --rm --network=none "$coordinator" \
  cat /opt/llm-cc-comparison/profile.json >"$OUTPUT/profile.json"

cat >"$OUTPUT/images.env" <<EOF
MODEL_IMAGE=$model
SCORER_IMAGE=$scorer
COORDINATOR_IMAGE=$coordinator
EOF
cat "$OUTPUT/images.env"
