#!/usr/bin/env bash
# Downloads happen before cleaning the local action cache. Run on idle hardware.
set -euo pipefail
result_dir="${1:?usage: benchmark_cuda.sh OUTPUT_DIRECTORY}"
mkdir -p "$result_dir"
result_dir="$(cd "$result_dir" && pwd)"
args=(--config=release --config=cuda --jobs="${JOBS:-16}" --disk_cache= --remote_cache= --remote_executor=)
# Analysis downloads the complete input closure but executes no compile actions.
bazel build "${args[@]}" --nobuild //:install
for configuration in portable a10; do
  extra=()
  if [[ "$configuration" == a10 ]]; then
    extra=(--//:cuda_archs=compute_86:sm_86)
  fi
  bazel clean
  for warmth in cold warm; do
    stem="$result_dir/$configuration-$warmth"
    bazel build "${args[@]}" "${extra[@]}" \
      --profile="$stem.profile.gz" --build_event_json_file="$stem.events.jsonl" \
      //:install > "$stem.log" 2>&1
    python3 tools/summarize_build_profile.py "$stem.profile.gz" "$stem.events.jsonl" > "$stem.json"
  done
done
