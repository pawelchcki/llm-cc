#!/usr/bin/env bash
set -euo pipefail

binary="$1"
model="$2"
source_file="$3"
output="$TEST_TMPDIR/model-smoke-output.jsonl"

"$binary" "$source_file" \
  --model "$model" \
  --backend cpu \
  --gpu-layers 0 >"$output"

if ! grep -Eq '"score":[[:space:]]*-?[0-9]' "$output"; then
  echo "model smoke test did not emit a numeric score" >&2
  exit 1
fi

"$binary" score --model "$model" --file "$source_file" \
  --force-cpu --progress always >"$output" 2>"$TEST_TMPDIR/score-progress.txt"

if ! grep -Eq '"log_probability":[[:space:]]*-?[0-9]' "$output"; then
  echo "scoring smoke test did not emit token probabilities" >&2
  exit 1
fi
if [[ "$(grep -c 'resolved backend=' "$TEST_TMPDIR/score-progress.txt")" != 1 ]]; then
  echo "scoring must initialize its backend exactly once" >&2
  exit 1
fi
