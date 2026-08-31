#!/usr/bin/env bash
set -euo pipefail

llm_cc_binary="${1:?missing Bazel llm-cc runfile}"
version_script_runfile="${2:?missing version script runfile}"
runfiles_source="${3:?missing Bazel runfiles root}"
shift 3

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
if [[ "$(uname -s)" == "Darwin" ]]; then
  mkdir -p "$bin_dir"
  installed_binary="$(mktemp "$bin_dir/.llm-cc.XXXXXXXX")"
  trap 'rm -f -- "$installed_binary"' EXIT
  install -m 0755 "$llm_cc_binary" "$installed_binary"
  mv "$installed_binary" "$bin_dir/llm-cc"
  trap - EXIT
  echo "Installed $("$bin_dir/llm-cc" --version) to $bin_dir/llm-cc"
  exit 0
fi

workspace="${BUILD_WORKSPACE_DIRECTORY:-}"
if [[ -n "$workspace" && -x "$workspace/tools/version.sh" ]]; then
  version_script="$workspace/tools/version.sh"
else
  version_script="$version_script_runfile"
fi
version="$("$version_script")"
libexec_root="$prefix/libexec/llm-cc"
if command -v sha256sum >/dev/null 2>&1; then
  hash_file() { sha256sum "$1" | cut -d' ' -f1; }
  hash_stream() { sha256sum | cut -d' ' -f1; }
elif command -v shasum >/dev/null 2>&1; then
  hash_file() { shasum -a 256 "$1" | cut -d' ' -f1; }
  hash_stream() { shasum -a 256 | cut -d' ' -f1; }
else
  echo "error: sha256sum or shasum is required to install" >&2
  exit 1
fi
payload_material="$(hash_file "$llm_cc_binary")"
if [[ -n "$workspace" ]]; then
  for build_input in \
      .bazelrc \
      BUILD.bazel \
      MODULE.bazel \
      build_defs.bzl \
      third_party/cuda_host_sysroot_package.patch \
      third_party/cuda_host_toolchain.BUILD.bazel \
      third_party/llama_cpp_deterministic_hip_cuid.patch \
      third_party/rocm_sdk.BUILD.bazel \
      tools/cuda_host_compiler_wrapper.sh \
      tools/gpu_sdk_repositories.bzl; do
    if [[ -f "$workspace/$build_input" ]]; then
      payload_material+="$(hash_file "$workspace/$build_input")"
    fi
  done
fi
payload_hash="$(printf '%s' "$payload_material" | hash_stream)"
payload_dir="$libexec_root/$version-${payload_hash:0:16}"
mkdir -p "$bin_dir" "$libexec_root"

if [[ ! -x "$payload_dir/llm-cc" ]]; then
  staging_dir="$(mktemp -d "$libexec_root/.install.XXXXXXXX")"
  trap 'rm -rf -- "$staging_dir"' EXIT
  install -m 0755 "$llm_cc_binary" "$staging_dir/llm-cc"

  # Shared accelerator builds rely on Bazel's checksum-pinned runfiles tree.
  # Copy it beside the executable so the binary's $ORIGIN-relative RPATH stays
  # valid after installation, independently of workspace convenience symlinks.
  if [[ -d "$runfiles_source" ]]; then
    if cp --version >/dev/null 2>&1; then
      cp -aL --reflink=auto "$runfiles_source" "$staging_dir/llm-cc.runfiles"
    else
      cp -aL "$runfiles_source" "$staging_dir/llm-cc.runfiles"
    fi
  fi

  mv "$staging_dir" "$payload_dir"
  trap - EXIT
fi

launcher="$(mktemp "$bin_dir/.llm-cc.XXXXXXXX")"
{
  printf '%s\n' '#!/usr/bin/env bash' 'set -euo pipefail'
  printf 'exec %q "$@"\n' "$payload_dir/llm-cc"
} > "$launcher"
chmod 0755 "$launcher"
mv "$launcher" "$bin_dir/llm-cc"
echo "Installed $("$bin_dir/llm-cc" --version) to $bin_dir/llm-cc"
