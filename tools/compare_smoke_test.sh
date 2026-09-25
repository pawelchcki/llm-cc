#!/usr/bin/env bash
# `llm-cc compare` with a real model: a cold run scores the changed files, a
# rerun is served entirely from the cache, and four workers or the separate CI
# stages reproduce the same results.
set -euo pipefail

binary="$1"
model="$2"
root="$TEST_TMPDIR/compare-smoke"
repo="$root/repo"
# Model digest memos stay inside the test.
export LLM_CC_CACHE_DIR="$root/cache/models"
mkdir -p "$repo/src" "$repo/tests" "$repo/tools"

git() { command git -C "$repo" -c user.name=smoke -c user.email=smoke@example.com "$@"; }
git init -q
cat >"$repo/src/math.cc" <<'EOF'
int Add(int a, int b) { return a + b; }
int Clamp(int value, int low, int high) {
  if (value < low) return low;
  if (value > high) return high;
  return value;
}
EOF
cat >"$repo/src/util.py" <<'EOF'
def pairs(values):
    return [(a, b) for a in values for b in values if a < b]
EOF
cat >"$repo/tests/math_test.cc" <<'EOF'
int main() { return Add(1, 2) == 3 ? 0 : 1; }
EOF
printf 'package tools\n\nfunc Name() string { return "smoke" }\n' >"$repo/tools/name.go"
git add .
git commit -qm base
cat >>"$repo/src/math.cc" <<'EOF'
int Sign(int value) {
  for (int i = 0; i < 1; ++i) {
    if (value > 0) return 1;
    if (value < 0) return -1;
  }
  return 0;
}
EOF
git mv src/util.py src/pairs.py
printf 'export function twice(x) { return x * 2; }\n' >"$repo/src/twice.js"
git add .
git commit -qm head

common=(--repo "$repo" --target HEAD~1 --model "$model" --force-cpu --progress never)
results() {
  python3 - "$1" <<'PY'
import json, sys
report = json.load(open(sys.argv[1] + "/report.json"))
if report["status"] == "failed":
    raise SystemExit("comparison failed: %s" % report["errors"])
print(json.dumps(report["results"], sort_keys=True))
PY
}

"$binary" compare run "${common[@]}" --cache "$root/store" --output-dir "$root/cold"
cold="$(results "$root/cold")"
python3 - "$root/cold" <<'PY'
import json, sys
directory = sys.argv[1]
plan = json.load(open(directory + "/plan.json"))
results = json.load(open(directory + "/report.json"))["results"]
if plan["scoring"]["backend"] != "cpu" or not plan["workers"]:
    raise SystemExit("a cold CPU run must plan workers")
if len(results) != len(plan["items"]) or not all(
        result["token_count"] > 0 for result in results.values()):
    raise SystemExit("every planned file must be scored with tokens")
PY

"$binary" compare run "${common[@]}" --cache "$root/store" --output-dir "$root/warm"
python3 - "$root/warm/plan.json" <<'PY'
import json, sys
plan = json.load(open(sys.argv[1]))
if plan["workers"] or plan["cache_stats"]["misses"] != 0:
    raise SystemExit("a rerun must be served from the cache")
PY
if [[ "$(results "$root/warm")" != "$cold" ]]; then
  echo "a cached rerun changed the results" >&2
  exit 1
fi

"$binary" compare run "${common[@]}" --cache "$root/four-store" \
  --max-workers 4 --output-dir "$root/four"
if [[ "$(results "$root/four")" != "$cold" ]]; then
  echo "four workers changed the results" >&2
  exit 1
fi

# The CI stages, with the model pinned by digest as a coordinator would.
printf '%s\n' --force-cpu --entropy-reduction host >"$root/scoring.args"
"$binary" compare identity --model "$model" "@$root/scoring.args" >"$root/identity.json"
read -r sha bytes < <(python3 -c 'import json,sys; m=json.load(open(sys.argv[1]))["model"]; print(m["sha256"], m["bytes"])' "$root/identity.json")
printf '{"repository": "smoke/repo", "pipeline_id": "1"}\n' >"$root/pipeline.json"
staged="$root/staged"
"$binary" compare prepare --repo "$repo" --head HEAD --target HEAD~1 \
  --identity "$root/pipeline.json" --output-dir "$staged" \
  --cache "$root/staged-store" --max-workers 2 \
  --model-sha256 "$sha" --model-bytes "$bytes" "@$root/scoring.args"
workers=()
for id in $(python3 -c 'import json,sys; print(" ".join(str(w["worker_id"]) for w in json.load(open(sys.argv[1]))["workers"]))' "$staged/plan.json"); do
  "$binary" compare worker --plan "$staged/plan.json" --worker-id "$id" \
    --output-dir "$staged" --cache "$root/staged-store" --model "$model" \
    --progress never
  workers+=(--worker "$staged/worker-$id.json")
done
"$binary" compare aggregate --plan "$staged/plan.json" "${workers[@]}" \
  --output-dir "$staged"
if [[ "$(results "$staged")" != "$cold" ]]; then
  echo "the CI stages changed the results" >&2
  exit 1
fi
