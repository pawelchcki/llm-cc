# CPU coordinator image: Git, the comparison package, object-store support and
# the installed-build identity, which is the scoring profile generated from the
# pushed scorer image. It holds no weights and no scorer.
#
# build.sh assembles the build context: tools/ from the pinned llm-cc commit,
# the generated profile.json and the default rules.json. One coordinator
# digest therefore pins the comparison code, the scoring contract and, through
# the profile, the exact GPU image every worker must run.
# Pin PYTHON_IMAGE by digest in production.
ARG PYTHON_IMAGE=docker.io/library/python:3.12-slim
FROM ${PYTHON_IMAGE}
ARG LLM_CC_COMMIT
# Publication markers use conditional PutObject (If-Match/If-None-Match),
# which needs a boto3 release from late 2024 or newer.
ARG BOTO3_VERSION=1.42.84
RUN echo "$LLM_CC_COMMIT" | grep -Eq '^[0-9a-f]{40}$' \
 || { echo "LLM_CC_COMMIT must name the pinned revision" >&2; exit 1; }
# CI checkouts belong to the runner's user, not this one.
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ca-certificates git \
 && rm -rf /var/lib/apt/lists/* \
 && git config --system --add safe.directory '*' \
 && pip install --no-cache-dir "boto3==${BOTO3_VERSION}" \
 && useradd --uid 10001 --user-group --home-dir /home/llm-cc --create-home llm-cc
COPY tools/ /opt/llm-cc-comparison/tools/
COPY profile.json rules.json /opt/llm-cc-comparison/
ENV HOME=/home/llm-cc \
    PYTHONPATH=/opt/llm-cc-comparison \
    PYTHONDONTWRITEBYTECODE=1
LABEL org.opencontainers.image.revision="${LLM_CC_COMMIT}"
WORKDIR /home/llm-cc
USER 10001:10001
