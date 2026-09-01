#!/usr/bin/env bash
set -euo pipefail

llm_cc_binary="${1:?missing Bazel llm-cc release executable}"
shift

prefix="${PREFIX:-${HOME:?HOME is unset}/.local}"
while (($# > 0)); do
  case "$1" in
    --prefix)
      (($# >= 2)) || { echo "error: --prefix requires a path" >&2; exit 2; }
      prefix="$2"
      shift 2
      ;;
    --prefix=*)
      prefix="${1#--prefix=}"
      shift
      ;;
    -h|--help)
      echo "Usage: bazel run //:install -- [--prefix PATH]"
      exit 0
      ;;
    *)
      echo "error: unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

if [[ -z "$prefix" ]]; then
  echo "error: install prefix cannot be empty" >&2
  exit 2
fi

bin_dir="$prefix/bin"
mkdir -p "$bin_dir"
installed_binary="$(mktemp "$bin_dir/.llm-cc.XXXXXXXX")"
trap 'rm -f -- "$installed_binary"' EXIT
install -m 0755 "$llm_cc_binary" "$installed_binary"
mv "$installed_binary" "$bin_dir/llm-cc"
trap - EXIT
echo "Installed $("$bin_dir/llm-cc" --version) to $bin_dir/llm-cc"
