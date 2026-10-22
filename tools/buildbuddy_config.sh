#!/usr/bin/env bash
# Prints the Bazel config for a BuildBuddy workflow step. Only push runs
# upload local results to the shared remote cache; other events only read it.
# With --upload-flag, prints the equivalent flag for a separate workspace.
set -euo pipefail
push=false
[[ "${BUILDBUDDY_TRIGGER_EVENT:-}" == push ]] && push=true
if [[ "${1:-}" == --upload-flag ]]; then
  "$push" || echo "--remote_upload_local_results=false"
else
  if "$push"; then echo buildbuddy; else echo buildbuddy-pr; fi
fi
