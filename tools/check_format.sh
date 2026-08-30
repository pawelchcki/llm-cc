#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
repo_root="$(dirname -- "$script_dir")"
cd "$repo_root"

files=()
while IFS= read -r -d '' file; do
  files+=("$file")
done < <(find src -type f \( -name '*.cc' -o -name '*.h' \) -print0)

if ((${#files[@]} == 0)); then
  echo "No C++ source files found under src/." >&2
  exit 1
fi

exec bazel run @llvm_toolchain_llvm//:bin/clang-format -- \
  --dry-run --Werror "${files[@]}"
