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
  --force-cpu --threads 2 --progress always >"$output" \
  2>"$TEST_TMPDIR/score-progress.txt"

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
  --threads 2 --flash-attn on --kv-cache-type q8_0 --progress never >"$output"
if ! grep -Eq '"log_probability":[[:space:]]*-?[0-9]' "$output"; then
  echo "Q8 K/V smoke test did not emit token probabilities" >&2
  exit 1
fi

# Force more than two independent inference windows, then compare the first,
# middle, and final windows with the same token ranges scored on their own.
window_source="$TEST_TMPDIR/window-source.cpp"
window_output="$TEST_TMPDIR/window-output.jsonl"
window_bos_output="$TEST_TMPDIR/window-bos-output.jsonl"
: >"$window_source"
for value in $(seq 1 12); do
  printf 'int window_value_%s = %s;\n' "$value" "$value" >>"$window_source"
done
"$binary" score --model "$model" --file "$window_source" --force-cpu \
  --threads 2 --bos never --context-size 25 --batch-size 7 --entropy \
  --progress never \
  >"$window_output"
"$binary" score --model "$model" --file "$window_source" --force-cpu \
  --threads 2 --bos always --context-size 25 --batch-size 7 --entropy \
  --progress never \
  >"$window_bos_output"

python3 - "$binary" "$model" "$window_source" "$window_output" \
  "$window_bos_output" "$TEST_TMPDIR" <<'PY'
import json
import math
import pathlib
import subprocess
import sys

binary, model, source_path, output_path, bos_output_path, temporary = sys.argv[1:]
context = 25
stride = context // 2
source = pathlib.Path(source_path).read_bytes()
records = [json.loads(line) for line in pathlib.Path(output_path).read_text().splitlines()]
if len(records) <= context + stride:
    raise SystemExit("window smoke input did not require more than two windows")
if [record["position"] for record in records] != list(range(len(records))):
    raise SystemExit("window scoring did not emit globally contiguous positions")
if records[0]["log_probability"] is not None:
    raise SystemExit("BOS-disabled scoring must emit one initial null record")
if any(record["log_probability"] is None for record in records[1:]):
    raise SystemExit("window boundaries introduced a null score")
if any(record["entropy"] is None for record in records[1:]):
    raise SystemExit("window boundaries introduced a null entropy")
pieces = [bytes.fromhex(record["bytes_hex"]) for record in records]
if b"".join(pieces) != source:
    raise SystemExit("window scoring did not preserve complete byte coverage")

bos_records = [json.loads(line) for line in pathlib.Path(bos_output_path).read_text().splitlines()]
if [record["position"] for record in bos_records] != list(range(len(bos_records))):
    raise SystemExit("BOS window scoring did not emit contiguous positions")
if any(record["log_probability"] is None or record["entropy"] is None
       for record in bos_records):
    raise SystemExit("BOS window scoring introduced a null record")
if b"".join(bytes.fromhex(record["bytes_hex"]) for record in bos_records) != source:
    raise SystemExit("BOS window scoring did not preserve complete byte coverage")
if len(bos_records) + 1 <= context + stride:
    raise SystemExit("BOS smoke input did not require more than two windows")

windows = []
covered = context
begin = 0
windows.append((begin, context, 1))
while covered < len(records):
    begin += stride
    end = min(len(records), begin + context)
    windows.append((begin, end, covered))
    covered = end
if len(windows) <= 2:
    raise SystemExit("window smoke planner produced too few windows")

sample_indices = sorted({0, len(windows) // 2, len(windows) - 1})
for window_index in sample_indices:
    begin, end, emit_begin = windows[window_index]
    manual_source = pathlib.Path(temporary) / f"manual-window-{window_index}.bin"
    manual_source.write_bytes(b"".join(pieces[begin:end]))
    completed = subprocess.run(
        [binary, "score", "--model", model, "--file", str(manual_source),
         "--force-cpu", "--threads", "2", "--bos", "never",
         "--context-size", str(context),
         "--batch-size", "7", "--entropy", "--progress", "never"],
        check=True, capture_output=True, text=True)
    manual = [json.loads(line) for line in completed.stdout.splitlines()]
    if len(manual) != end - begin:
        raise SystemExit("manual window did not reproduce its token range")
    for target in range(emit_begin, end):
        expected = manual[target - begin]
        actual = records[target]
        if (expected["token_id"] != actual["token_id"] or
                expected["bytes_hex"] != actual["bytes_hex"]):
            raise SystemExit("manual window tokenization differs from window scoring")
        if not math.isclose(expected["log_probability"],
                            actual["log_probability"],
                            rel_tol=1e-5, abs_tol=1e-6):
            raise SystemExit("manual window probability differs from window scoring")
        if not math.isclose(expected["entropy"], actual["entropy"],
                            rel_tol=1e-5, abs_tol=1e-6):
            raise SystemExit("manual window entropy differs from window scoring")
PY
