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

# Exercise exact context growth, reuse, and K/V clearing in both file orders.
small="$TEST_TMPDIR/context-small.cpp"
large="$TEST_TMPDIR/context-large.cpp"
cp "$source_file" "$small"
: >"$large"
for _ in $(seq 1 8); do
  cat "$source_file" >>"$large"
done
context_case=0
for order in "$small $large" "$large $small"; do
  context_case=$((context_case + 1))
  context_output="$TEST_TMPDIR/context-output-$context_case.jsonl"
  # Word splitting is intentional: TEST_TMPDIR paths contain no spaces in the
  # Bazel harness, and the two explicit sources must remain separate argv items.
  # shellcheck disable=SC2086
  "$binary" $order --model "$model" --force-cpu --no-cache \
    --context 8192 --batch-size 64 --progress always >"$context_output" \
    2>"$TEST_TMPDIR/context-progress-$context_case.txt"
  if [[ "$(grep -c '"type":"file"' "$context_output")" != 2 ]]; then
    echo "context smoke did not score both files" >&2
    exit 1
  fi
done
if ! grep -q 'resetting inference context requested=.*allocated=' \
    "$TEST_TMPDIR/context-progress-2.txt"; then
  echo "context smoke did not clear and reuse a sufficient context" >&2
  exit 1
fi
grep '"type":"file"' "$TEST_TMPDIR/context-output-1.jsonl" | sort \
  >"$TEST_TMPDIR/context-files-1.jsonl"
grep '"type":"file"' "$TEST_TMPDIR/context-output-2.jsonl" | sort \
  >"$TEST_TMPDIR/context-files-2.jsonl"
if ! cmp -s "$TEST_TMPDIR/context-files-1.jsonl" \
    "$TEST_TMPDIR/context-files-2.jsonl"; then
  echo "context reset changed scores when file order was reversed" >&2
  exit 1
fi

# Quantized K and V are validated together and require Flash Attention.
"$binary" score --model "$model" --file "$source_file" --force-cpu \
  --flash-attn on --kv-cache-type q8_0 --progress never >"$output"
if ! grep -Eq '"log_probability":[[:space:]]*-?[0-9]' "$output"; then
  echo "Q8 K/V smoke test did not emit token probabilities" >&2
  exit 1
fi
