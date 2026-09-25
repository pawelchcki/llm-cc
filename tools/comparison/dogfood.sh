#!/usr/bin/env bash
set -euo pipefail
# The coordinator derives head, branch, and default branch from the checkout.
exec python3 -m tools.comparison.submit_bazzite run-coordinator \
  --config "${LLM_CC_COMPARISON_CONFIG:-/var/lib/llm-cc/comparison.json}" \
  --repository pawelchcki/llm-cc
