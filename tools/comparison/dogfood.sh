#!/usr/bin/env bash
set -euo pipefail
head="${COMMIT_SHA:-$(git rev-parse HEAD)}"
branch="${GIT_BRANCH:-$(git branch --show-current)}"
if [[ -z "$branch" ]]; then
  branch="$(python3 -c 'from tools.comparison.submit_bazzite import runner_metadata; print(runner_metadata("BRANCH_NAME"))')"
fi
# BuildBuddy's checkout may be a synthetic merge; compare the actual PR head.
if [[ "$(git show -s --format=%ce "$head")" == ci-runner@buildbuddy.io ]]; then
  head="$(git rev-parse "$head^1")"
fi
exec python3 -m tools.comparison.submit_bazzite run-coordinator \
  --config "${LLM_CC_COMPARISON_CONFIG:-/etc/llm-cc/comparison.json}" \
  --repository pawelchcki/llm-cc --head "$head" --branch "$branch"
