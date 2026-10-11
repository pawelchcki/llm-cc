#!/usr/bin/env bash
set -euo pipefail
# The installed coordinator verifies its package checksum and isolates imports;
# PR files cannot replace the measurement producer or its Python dependencies.
/var/lib/llm-cc/bin/llm-cc-coordinate \
  --config "${LLM_CC_COMPARISON_CONFIG:-/var/lib/llm-cc/comparison-v2.json}" \
  --repository pawelchcki/llm-cc
python3 -I tools/comparison/ratchet.py "$BUILDBUDDY_ARTIFACTS_DIRECTORY"
