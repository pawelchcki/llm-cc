#!/usr/bin/env bash
# Regenerate the tau-calibration results end to end.
#
#   LLM_CC_BINARY=bazel-bin/llm-cc LLM_CC_INFERENCE='--backend rocm' \
#     experiments/tau-calibration/regenerate.sh [--reference] [--publish]
#
# Results are written to the `results` submodule, which tracks the protected
# `data/tau-calibration` branch. Existing entropy dumps are reused; delete one
# to recompute it. --reference reruns the authors' CodeLlama-7b-hf pipeline
# (needs torch, transformers, tree_sitter). --publish commits the results to
# the data branch and stages the new submodule pointer and summary here.
set -euo pipefail

here="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
binary="${LLM_CC_BINARY:?set LLM_CC_BINARY to the llm-cc binary}"
inference="${LLM_CC_INFERENCE:-}"
reference=false
publish=false
for arg in "$@"; do
  case "$arg" in
    --reference) reference=true ;;
    --publish) publish=true ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done

cd "$here"
git submodule update --init --checkout results

python3 prepare.py
if "$reference"; then
  python3 reference_anchor.py --dtype float16
fi
python3 run.py entropy --binary "$binary" --inference="$inference"
python3 summarize.py
python3 run.py analysis --binary "$binary" --inference="$inference"
python3 summarize.py

if "$publish"; then
  git -C results add -A
  if git -C results diff --cached --quiet; then
    echo "results unchanged"
  else
    git -C results commit -m "data: regenerate tau-calibration results"
    git -C results push origin HEAD:refs/heads/data/tau-calibration
  fi
  git add results summary.json
  echo "Staged the submodule pointer and summary.json; tag the data commit" \
    "as data/tau-calibration-vN before merging."
fi
