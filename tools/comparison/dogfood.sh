#!/usr/bin/env bash
set -euo pipefail
# llm-cc aggregates the reports, so render them with this checkout's own build
# and pull requests exercise aggregation changes as well. Runs never overlap, so
# one output base in the executor's home keeps rebuilds incremental.
if [[ -z "${LLM_CC:-}" ]]; then
  output_base="${HOME:?}/.cache/llm-cc-dogfood-bazel"
  bazel --output_base="$output_base" build --config=buildbuddy \
    --remote_download_toplevel //:llm-cc
  LLM_CC="$(readlink -f bazel-bin/llm-cc)"
  export LLM_CC
fi
# The coordinator derives head, branch, and default branch from the checkout.
exec python3 -m tools.comparison.submit_bazzite run-coordinator \
  --config "${LLM_CC_COMPARISON_CONFIG:-/var/lib/llm-cc/comparison.json}" \
  --repository pawelchcki/llm-cc
