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
bazel_bin="$(bazel info bazel-bin 2>/dev/null)"
llama_root="$output_base/external/+http_archive+llama_cpp"
json_root="$output_base/external/+http_archive+nlohmann_json"
tree_sitter_root="$output_base/external/+http_archive+tree_sitter"
curl_root="$output_base/external/+http_archive+curl"
fixes_file="$(mktemp "${TMPDIR:-/tmp}/llm-cc-clang-tidy.XXXXXX")"
trap 'rm -f "$fixes_file"' EXIT

platform_args=()
if [[ "$(uname -s)" == Darwin ]]; then
  platform_args=(-isysroot "$(xcrun --sdk macosx --show-sdk-path)")
fi

bazel run @llvm_toolchain_llvm//:bin/clang-tidy -- \
  --export-fixes="$fixes_file" "${files[@]}" -- -x c++ -std=c++20 \
  "${platform_args[@]}" -iquote . \
  -I"$bazel_bin" -isystem "$llama_root/include" \
  -isystem "$llama_root/ggml/include" \
  -isystem "$json_root/single_include" \
  -isystem "$tree_sitter_root/lib/include" -isystem "$curl_root/include"

if [[ -s "$fixes_file" ]] && ! grep -q '^Diagnostics: \[\]$' "$fixes_file"; then
  echo "clang-tidy reported diagnostics." >&2
  exit 1
fi
