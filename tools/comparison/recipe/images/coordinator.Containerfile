# CPU coordinator image: Git, the provider glue (discovery, CI generation,
# publication) with boto3, the scorer's own llm-cc, which prepares plans and
# aggregates reports, and the scoring contract. It holds no weights and runs
# no inference.
#
# build.sh assembles the build context: tools/ from the pinned llm-cc commit,
# scoring.args (scoring settings and the model pin, one argument per line),
# planning.args (the file size limit) and the default rules.json. One coordinator digest therefore pins the glue, the
# scoring contract and, through LLM_CC_SCORER_IMAGE, the exact GPU image every
# worker must run; that image's llm-cc refuses a plan from any other build.
# Pin PYTHON_IMAGE by digest in production. SCORER_IMAGE is the pushed scorer
# by digest.
ARG PYTHON_IMAGE=docker.io/library/python:3.12-slim
ARG SCORER_IMAGE
FROM ${SCORER_IMAGE} AS scorer

FROM ${PYTHON_IMAGE}
ARG LLM_CC_COMMIT
ARG SCORER_IMAGE
# Publication markers use conditional PutObject (If-Match/If-None-Match),
# which needs a boto3 release from late 2024 or newer.
ARG BOTO3_VERSION=1.42.84
RUN echo "$LLM_CC_COMMIT" | grep -Eq '^[0-9a-f]{40}$' \
 || { echo "LLM_CC_COMMIT must name the pinned revision" >&2; exit 1; }
# PYTHONSAFEPATH below needs Python 3.11 or newer.
RUN python3 -c 'import sys; sys.exit(sys.version_info < (3, 11))' \
 || { echo "PYTHON_IMAGE must provide Python 3.11 or newer" >&2; exit 1; }
# CI checkouts belong to the runner's user, not this one.
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ca-certificates git \
 && rm -rf /var/lib/apt/lists/* \
 && git config --system --add safe.directory '*' \
 && pip install --no-cache-dir "boto3==${BOTO3_VERSION}" \
 && useradd --uid 10001 --user-group --home-dir /home/llm-cc --create-home llm-cc
COPY --from=scorer /opt/llm-cc/bin/llm-cc /usr/local/bin/llm-cc
COPY tools/ /opt/llm-cc-comparison/tools/
COPY scoring.args planning.args rules.json /opt/llm-cc-comparison/
# Fail the build now if this llm-cc rejects the scoring contract.
RUN llm-cc compare identity @/opt/llm-cc-comparison/scoring.args >/dev/null
# Jobs run `python3 -m tools.comparison` inside the project checkout.
# PYTHONSAFEPATH keeps the working directory off sys.path, so a checkout with
# its own tools/ package (llm-cc itself) cannot replace the pinned code.
ENV HOME=/home/llm-cc \
    LLM_CC_SCORER_IMAGE=${SCORER_IMAGE} \
    PYTHONPATH=/opt/llm-cc-comparison \
    PYTHONSAFEPATH=1 \
    PYTHONDONTWRITEBYTECODE=1
LABEL org.opencontainers.image.revision="${LLM_CC_COMMIT}"
WORKDIR /home/llm-cc
USER 10001:10001
