# GPU scorer image: the installed llm-cc CUDA scorer and a non-root user, built
# on the content-keyed model image by digest. `llm-cc compare worker` is the
# whole worker: it reads the plan, scores, and stores results in S3 itself.
#
# Build from the pinned commit's tracked files (build.sh uses `git archive`):
#
#   podman build -f tools/comparison/recipe/images/scorer.Containerfile \
#     --build-arg MODEL_IMAGE=registry.example.com/llm-cc/model@sha256:<digest> \
#     --build-arg LLM_CC_COMMIT=<40-hex commit> --build-arg LLM_CC_VERSION=<version> \
#     --build-arg BAZELISK_URL=<bazelisk-linux-amd64 URL> \
#     --build-arg BAZELISK_SHA256=<its SHA-256> \
#     --secret id=bazelrc,src=remote-cache.bazelrc .
#
# The optional bazelrc secret can point Bazel at a remote cache. It is mounted
# for the build step only, never stored in a layer or a build argument, so a
# local build needs no organizational credentials. Pin BUILDER_IMAGE and
# RUNTIME_IMAGE by digest in production.
ARG MODEL_IMAGE
ARG BUILDER_IMAGE=docker.io/library/ubuntu:24.04
ARG RUNTIME_IMAGE=docker.io/library/ubuntu:24.04

FROM ${BUILDER_IMAGE} AS build
ARG MODEL_IMAGE
ARG LLM_CC_COMMIT
ARG LLM_CC_VERSION
ARG CUDA_ARCHS=compute_86:sm_86
ARG BAZELISK_URL
ARG BAZELISK_SHA256
# Layer reuse, and the result cache's fingerprint, need immutable inputs.
RUN echo "$MODEL_IMAGE" | grep -Eq '^[^[:space:]@]+@sha256:[0-9a-f]{64}$' \
 || { echo "MODEL_IMAGE must be pinned as name@sha256:<digest>" >&2; exit 1; }
RUN echo "$LLM_CC_COMMIT" | grep -Eq '^[0-9a-f]{40}$' && test -n "$LLM_CC_VERSION" \
 || { echo "LLM_CC_COMMIT and LLM_CC_VERSION must name the pinned revision" >&2; exit 1; }
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates libxml2 libstdc++6 zlib1g python3 git perl xz-utils bzip2 unzip curl \
 && rm -rf /var/lib/apt/lists/*
RUN curl --fail --location --proto '=https' --output /usr/local/bin/bazel "$BAZELISK_URL" \
 && echo "$BAZELISK_SHA256  /usr/local/bin/bazel" | sha256sum -c - \
 && chmod 0755 /usr/local/bin/bazel
WORKDIR /src
COPY . /src
# Explicit provenance flags record llm-cc's own revision; the image build
# never substitutes whatever Git state the build context happens to carry.
RUN --mount=type=secret,id=bazelrc,required=false \
    set -eu; \
    rc=; if [ -s /run/secrets/bazelrc ]; then rc=--bazelrc=/run/secrets/bazelrc; fi; \
    bazel $rc run --config=release --config=cuda \
      --//:cuda_archs="$CUDA_ARCHS" \
      --//:source_commit="$LLM_CC_COMMIT" \
      --//:source_version="$LLM_CC_VERSION" \
      //:install -- --prefix /opt/llm-cc; \
    bazel $rc shutdown
# Fail the build now if the installed scorer cannot state its own identity.
RUN /opt/llm-cc/bin/llm-cc compare identity --backend cuda \
      --entropy-reduction device --flash-attn on \
      --model-sha256 0000000000000000000000000000000000000000000000000000000000000000 \
      --model-bytes 1 >/dev/null

FROM ${RUNTIME_IMAGE} AS runtime
# llm-cc talks to S3 itself; the image needs no Python or SDK.
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --uid 10001 --user-group --home-dir /home/llm-cc --create-home llm-cc
COPY --from=build /opt/llm-cc /opt/llm-cc

# Start from the model image so its layer is shared by digest with every
# scorer built on the same weights; the whole runtime is one layer above it.
FROM ${MODEL_IMAGE}
ARG MODEL_IMAGE
ARG LLM_CC_COMMIT
ARG LLM_CC_VERSION
ARG CUDA_ARCHS=compute_86:sm_86
COPY --from=runtime / /
ENV PATH=/opt/llm-cc/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    HOME=/home/llm-cc \
    NVIDIA_VISIBLE_DEVICES=all \
    NVIDIA_DRIVER_CAPABILITIES=compute,utility
LABEL org.opencontainers.image.revision="${LLM_CC_COMMIT}" \
      org.opencontainers.image.version="${LLM_CC_VERSION}" \
      io.llm-cc.model-image="${MODEL_IMAGE}" \
      io.llm-cc.cuda-archs="${CUDA_ARCHS}"
WORKDIR /home/llm-cc
# Workers score untrusted repository content: never as root.
USER 10001:10001
