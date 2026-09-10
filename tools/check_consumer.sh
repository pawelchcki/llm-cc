#!/usr/bin/env bash
# Run outside Bazel: this test creates an independent module and action graph.
set -euo pipefail
source_root="$(cd -- "$(dirname -- "$0")/.." && pwd)"
consumer="$(mktemp -d "${TMPDIR:-/tmp}/llm-cc-consumer.XXXXXXXX")"
trap 'rm -rf "$consumer"' EXIT
cp -R "$source_root/examples/consumer/." "$consumer/"
cp "$source_root/.bazelversion" "$consumer/"
for patch in rules_cuda_explicit_tools rules_foreign_cc_reproducible_logs; do
  cmp "$source_root/third_party/$patch.patch" "$consumer/patches/$patch.patch"
done
python3 - "$consumer" "$source_root" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
for name in ["MODULE.bazel", ".bazelrc"]:
    path = root / name
    text = path.read_text().replace('path = "../.."', 'path = ' + repr(sys.argv[2]))
    text = text.replace('version = "0.1.0")', 'version = "0.1.0", repo_name = "renamed_engine")')
    path.write_text(text.replace("@llm_cc//", "@renamed_engine//"))
PY
cd "$consumer"
git init -q
git -c user.name=Fixture -c user.email=fixture@example.invalid commit -q --allow-empty -m 'Consumer identity'
cat > status.sh <<'STATUS'
#!/bin/sh
echo STABLE_LLM_CC_VERSION 99.0.0
echo STABLE_LLM_CC_GIT_SHA aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
STATUS
backend="${BACKEND:-cpu}"
args=(--config=release --config="$backend" --stamp --workspace_status_command='bash status.sh'
  --@renamed_engine//:source_version=0.2.0-consumer-fixture
  --@renamed_engine//:source_commit=a300b37b912a06f79869582b7f870eace005b329)
if [[ -n "${CUDA_ARCHS:-}" ]]; then
  args+=("--@renamed_engine//:cuda_archs=$CUDA_ARCHS")
fi
bazel build "${args[@]}" @renamed_engine//:install
execution_root="$(bazel info execution_root)"
launcher="$execution_root/$(bazel cquery "${args[@]}" @renamed_engine//:install --output=files)"
# Bazel may insert an empty _main/.runfile marker. Deliberately omit that
# directory to exercise external short paths against the runfiles root itself.
mkdir "$consumer/runfiles"
for entry in "$launcher.runfiles/"* "$launcher.runfiles/".[!.]*; do
  [[ -e "$entry" ]] || continue
  [[ "${entry##*/}" == _main ]] && continue
  cp -R "$entry" "$consumer/runfiles/"
done
[[ ! -e "$consumer/runfiles/_main" ]]
umask 077
RUNFILES_DIR="$consumer/runfiles" "$launcher" --prefix "$consumer/prefix"
[[ "$("$consumer/prefix/bin/llm-cc" --version)" == *0.2.0-consumer-fixture* ]]
key="$(LLM_CC_RUNTIME_DIR="$consumer/prefix/lib/llm-cc" "$consumer/prefix/bin/llm-cc" backends path cuda)"
[[ "$key" == *a300b37b912a06f79869582b7f870eace005b329* ]]
if [[ "$backend" == cuda ]]; then
  LLM_CC_RUNTIME_DIR="$consumer/prefix/lib/llm-cc" "$consumer/prefix/bin/llm-cc" backends fetch cuda --no-download
fi
