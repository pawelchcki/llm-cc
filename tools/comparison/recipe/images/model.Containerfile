# Content-keyed model image: the verified GGUF weights and nothing else.
#
# Tag it with the weights' SHA-256 and build scorer images FROM it by digest.
# Scorer rebuilds then reuse this multi-gigabyte layer instead of
# redistributing the weights; a fresh GPU node still pulls it once.
#
#   podman build -f model.Containerfile \
#     --build-arg MODEL_URL=https://huggingface.co/bartowski/DeepSeek-Coder-V2-Lite-Base-GGUF/resolve/babc004c32d74af2553661d912673f1d22dcb3f1/DeepSeek-Coder-V2-Lite-Base-Q6_K.gguf \
#     --build-arg MODEL_SHA256=5a2e25280075d769abdb111de8211d9d3367f2ae0d0e6166a288ee6e8ed0345d \
#     --build-arg MODEL_BYTES=14066972416 \
#     --tag registry.example.com/llm-cc/model:5a2e25280075d769abdb111de8211d9d3367f2ae0d0e6166a288ee6e8ed0345d .
#
# Pin FETCH_IMAGE by digest in production; it never reaches the final image.
ARG FETCH_IMAGE=docker.io/library/alpine:3.20

FROM ${FETCH_IMAGE} AS fetch
ARG MODEL_URL
ARG MODEL_SHA256
ARG MODEL_BYTES
RUN apk add --no-cache curl
# Refuse to download more than the pinned size, then verify size and digest.
RUN set -eu; \
    test -n "$MODEL_URL" && test -n "$MODEL_BYTES"; \
    echo "$MODEL_SHA256" | grep -Eq '^[0-9a-f]{64}$'; \
    mkdir /models; \
    curl --fail --location --proto '=https' --proto-redir '=https' --retry 3 \
      --max-filesize "$MODEL_BYTES" --output /models/model.gguf "$MODEL_URL"; \
    test "$(stat -c %s /models/model.gguf)" = "$MODEL_BYTES"; \
    echo "$MODEL_SHA256  /models/model.gguf" | sha256sum -c -; \
    chmod 0444 /models/model.gguf

FROM scratch
ARG MODEL_SHA256
ARG MODEL_BYTES
COPY --from=fetch /models/model.gguf /models/model.gguf
LABEL io.llm-cc.model.sha256="${MODEL_SHA256}" \
      io.llm-cc.model.bytes="${MODEL_BYTES}"
