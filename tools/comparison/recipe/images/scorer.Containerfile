# GPU scorer image: the installed llm-cc CUDA scorer, the comparison worker and
# a non-root user, built on the content-keyed model image by digest.
#
# Build from an llm-cc checkout at the pinned commit (build.sh does this):
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
# Fail the build now if the installed tree cannot state its own identity. The
# real profile names this image's digest, so build.sh generates it after push.
RUN PYTHONPATH=/src python3 -m tools.comparison.profile generate \
      --installed-root /opt/llm-cc --backend cuda \
      --execution-image registry.invalid/llm-cc-scorer@sha256:0000000000000000000000000000000000000000000000000000000000000000 \
      --model-sha256 0000000000000000000000000000000000000000000000000000000000000000 \
      --model-bytes 1 --max-file-bytes 49152 --output /dev/null

FROM ${RUNTIME_IMAGE} AS runtime
# Workers only read and write whole objects, so the distribution's boto3 is
# sufficient here; the coordinator needs conditional writes.
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates python3 python3-boto3 \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --uid 10001 --user-group --home-dir /home/llm-cc --create-home llm-cc
COPY --from=build /opt/llm-cc /opt/llm-cc
COPY --from=build /src/tools/__init__.py /opt/llm-cc-comparison/tools/__init__.py
COPY --from=build /src/tools/comparison/*.py /opt/llm-cc-comparison/tools/comparison/

# Start from the model image so its layer is shared by digest with every
# scorer built on the same weights; the whole runtime is one layer above it.
FROM ${MODEL_IMAGE}
ARG MODEL_IMAGE
ARG LLM_CC_COMMIT
ARG LLM_CC_VERSION
ARG CUDA_ARCHS=compute_86:sm_86
COPY --from=runtime / /
ENV PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    HOME=/home/llm-cc \
    PYTHONPATH=/opt/llm-cc-comparison \
    PYTHONDONTWRITEBYTECODE=1 \
    NVIDIA_VISIBLE_DEVICES=all \
    NVIDIA_DRIVER_CAPABILITIES=compute,utility
LABEL org.opencontainers.image.revision="${LLM_CC_COMMIT}" \
      org.opencontainers.image.version="${LLM_CC_VERSION}" \
      io.llm-cc.model-image="${MODEL_IMAGE}" \
      io.llm-cc.cuda-archs="${CUDA_ARCHS}"
WORKDIR /home/llm-cc
# Workers score untrusted repository content: never as root.
USER 10001:10001
