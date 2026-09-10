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
# Resolve apparent repository names through Bazel's module mapping.
repo_mapping="$(bazel mod dump_repo_mapping '')"
repo_path() {
  python3 -c 'import json,sys; print(sys.argv[2] + "/external/" + json.loads(sys.argv[3])[sys.argv[1]])' "$1" "$output_base" "$repo_mapping"
}
llama_root="$(repo_path llama_cpp)"
json_root="$(repo_path nlohmann_json)"
tree_sitter_root="$(repo_path tree_sitter)"
curl_root="$(repo_path curl)"
llvm_root="$(repo_path llvm_toolchain_llvm)"
boringssl_root="$(repo_path boringssl)"
fixes_file="$(mktemp "${TMPDIR:-/tmp}/llm-cc-clang-tidy.XXXXXX")"
trap 'rm -f "$fixes_file"' EXIT

platform_args=()
if [[ "$(uname -s)" == Darwin ]]; then
  platform_args=(-isysroot "$(xcrun --sdk macosx --show-sdk-path)")
elif [[ "$(uname -s)" == Linux ]]; then
  sysroot="$(repo_path linux_glibc_sysroot)"
  platform_args=(
    --target=x86_64-unknown-linux-gnu
    -resource-dir "$llvm_root/lib/clang/22"
    -stdlib=libc++
    --sysroot="$sysroot"
    -nostdinc++
    -cxx-isystem "$llvm_root/include/c++/v1"
    -cxx-isystem "$llvm_root/include/x86_64-unknown-linux-gnu/c++/v1"
    -idirafter "$llvm_root/lib/clang/22/include"
  )
fi

bazel run @llvm_toolchain_llvm//:bin/clang-tidy -- \
  --export-fixes="$fixes_file" "${files[@]}" -- -x c++ -std=c++20 \
  "${platform_args[@]}" -iquote . \
  -I"$bazel_bin" -isystem "$llama_root/include" \
  -isystem "$llama_root/ggml/include" \
  -isystem "$json_root/single_include" \
  -isystem "$tree_sitter_root/lib/include" -isystem "$curl_root/include" -isystem "$boringssl_root/include"

if [[ -s "$fixes_file" ]] && ! grep -q '^Diagnostics: \[\]$' "$fixes_file"; then
  echo "clang-tidy reported diagnostics." >&2
  exit 1
fi
