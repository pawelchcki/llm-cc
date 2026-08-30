#!/usr/bin/env bash
set -euo pipefail

rethink_binary="${1:?missing Bazel rethink-cc runfile}"
version_script_runfile="${2:?missing version script runfile}"
shift 2

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

workspace="${BUILD_WORKSPACE_DIRECTORY:-}"
if [[ -n "$workspace" && -x "$workspace/tools/version.sh" ]]; then
  version_script="$workspace/tools/version.sh"
else
  version_script="$version_script_runfile"
fi
version="$("$version_script")"
bin_dir="$prefix/bin"
mkdir -p "$bin_dir"
install -m 0755 "$rethink_binary" "$bin_dir/rethink-cc"
echo "Installed $("$bin_dir/rethink-cc" --version) to $bin_dir/rethink-cc"

if [[ -z "$workspace" ]]; then
  workspace="$(CDPATH= cd -- "$(dirname -- "$version_script_runfile")/.." && pwd)"
fi
lmcc_binary="$workspace/lmcc/target/release/lmcc"

if command -v cargo >/dev/null 2>&1; then
  echo "Building lmcc $version with Cargo..."
  CARGO_TARGET_DIR="$workspace/lmcc/target" \
    cargo build --release --package lmcc-cli \
      --manifest-path "$workspace/lmcc/Cargo.toml"
fi

if [[ -x "$lmcc_binary" ]]; then
  lmcc_version="$("$lmcc_binary" --version)"
  install -m 0755 "$lmcc_binary" "$bin_dir/lmcc"
  echo "Installed $lmcc_version to $bin_dir/lmcc"
  if [[ "$lmcc_version" != "lmcc $version" ]]; then
    echo "Warning: lmcc reports '$lmcc_version', expected 'lmcc $version'." >&2
  fi
else
  echo "Skipped lmcc: Cargo is unavailable and $lmcc_binary is not built." >&2
fi
