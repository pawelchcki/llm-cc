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

output_base="$(bazel info output_base 2>/dev/null)"
llama_root="$output_base/external/+http_archive+llama_cpp"
fixes_file="$(mktemp "${TMPDIR:-/tmp}/rethink-clang-tidy.XXXXXX")"
trap 'rm -f "$fixes_file"' EXIT

bazel run @llvm_toolchain_llvm//:bin/clang-tidy -- \
  --export-fixes="$fixes_file" "${files[@]}" -- -x c++ -std=c++20 -I. \
  -isystem "$llama_root/include" -isystem "$llama_root/ggml/include"

if [[ -s "$fixes_file" ]] && ! grep -q '^Diagnostics: \[\]$' "$fixes_file"; then
  echo "clang-tidy reported diagnostics." >&2
  exit 1
fi
