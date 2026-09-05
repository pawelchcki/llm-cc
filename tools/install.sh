#!/usr/bin/env bash
set -euo pipefail

llm_cc_binary="${1:?missing Bazel llm-cc release executable}"
shift

prefix="${PREFIX:-${HOME:?HOME is unset}/.local}"
bundle_sources=()
checksum_sources=()
manifest_sources=()
while (($# > 0)); do
  case "$1" in
    --bundle|--checksum|--manifest)
      (($# >= 2)) || { echo "error: $1 requires a path" >&2; exit 2; }
      case "$1" in
        --bundle) bundle_sources+=("$2") ;;
        --checksum) checksum_sources+=("$2") ;;
        --manifest) manifest_sources+=("$2") ;;
      esac
      shift 2
      ;;
    --)
      shift
      break
      ;;
    *)
      echo "error: invalid installer input: $1" >&2
      exit 2
      ;;
  esac
done

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
if ((${#bundle_sources[@]} != ${#checksum_sources[@]} ||
     ${#bundle_sources[@]} != ${#manifest_sources[@]})); then
  echo "error: each backend bundle needs its checksum and manifest" >&2
  exit 2
fi

bin_dir="$prefix/bin"
install_root="$prefix/lib/llm-cc"
mkdir -p "$bin_dir" "$install_root"
work_dir="$(mktemp -d "$prefix/.llm-cc-install.XXXXXXXX")"
stage_dir=""
bundle_dir=""
old_bundle_dir=""
installed_binary="$bin_dir/llm-cc"
old_binary=""
committed=0
old_bundle_moved=0
bundle_set_installed=0
old_binary_moved=0
binary_installed=0
rollback() {
  # The old executable and bundle set remain recoverable until both swaps have
  # completed. This also handles an interruption between either pair of moves.
  if ((binary_installed)) && [[ -e "$installed_binary" ]]; then
    if ! mv -- "$installed_binary" "$work_dir/rejected-llm-cc"; then
      return 1
    fi
  fi
  if ((old_binary_moved)) && ! mv -- "$old_binary" "$installed_binary"; then
    return 1
  fi
  if ((old_bundle_moved)); then
    if ((bundle_set_installed)) && [[ -e "$bundle_dir" ]]; then
      if ! mv -- "$bundle_dir" "$work_dir/rejected-bundles"; then
        return 1
      fi
    fi
    if ! mv -- "$old_bundle_dir" "$bundle_dir"; then
      return 1
    fi
  elif ((bundle_set_installed)) && [[ -e "$bundle_dir" ]]; then
    if ! mv -- "$bundle_dir" "$work_dir/rejected-bundles"; then
      return 1
    fi
  fi
}
on_exit() {
  status=$?
  if ((status != 0 && committed == 0)); then
    if ! rollback; then
      echo "error: installation rollback failed; recover from $work_dir or $old_bundle_dir" >&2
      trap - EXIT
      exit "$status"
    fi
  fi
  [[ -z "$stage_dir" ]] || rm -rf -- "$stage_dir"
  rm -rf -- "$work_dir"
  exit "$status"
}
trap on_exit EXIT

# Check that the replacement executable starts before changing either active
# bundles or the old executable.
staged_binary="$work_dir/llm-cc"
install -m 0755 "$llm_cc_binary" "$staged_binary"
"$staged_binary" --version >/dev/null

# The executable is the single authority for its build key. Giving its normal
# cache command an installed root yields lib/llm-cc/backends/<build-key>.
backend_path="$(LLM_CC_RUNTIME_DIR="$install_root" "$staged_binary" backends path cuda)"
if [[ -z "$backend_path" || "$(dirname -- "$backend_path")" == "." ]]; then
  echo "error: staged executable returned an invalid backend path" >&2
  exit 1
fi
bundle_dir="$(dirname -- "$backend_path")"
bundle_parent="$(dirname -- "$bundle_dir")"
mkdir -p "$bundle_parent"
stage_dir="$(mktemp -d "$bundle_parent/.${bundle_dir##*/}.incoming.XXXXXXXX")"

for index in "${!bundle_sources[@]}"; do
  source_bundle="${bundle_sources[index]}"
  source_checksum="${checksum_sources[index]}"
  source_manifest="${manifest_sources[index]}"
  for source in "$source_bundle" "$source_checksum" "$source_manifest"; do
    [[ -f "$source" ]] || { echo "error: backend install input is missing: $source" >&2; exit 1; }
  done
  case "$(basename -- "$source_bundle")" in
    *cuda*.bundle) backend=cuda ;;
    *rocm*.bundle) backend=rocm ;;
    *) echo "error: cannot identify backend bundle: $source_bundle" >&2; exit 1 ;;
  esac
  destination="$(LLM_CC_RUNTIME_DIR="$install_root" "$staged_binary" backends path "$backend")"
  if [[ "$(dirname -- "$destination")" != "$bundle_dir" ]]; then
    echo "error: staged executable produced inconsistent backend paths" >&2
    exit 1
  fi
  install -m 0644 "$source_bundle" "$stage_dir/$backend.bundle"
  install -m 0644 "$source_checksum" "$stage_dir/$backend.bundle.sha256"
  install -m 0644 "$source_manifest" "$stage_dir/$backend.manifest.json"
done

# Swap a complete set, including an empty CPU/Metal set, so a backend change
# cannot leave a stale same-build GPU bundle active.
old_bundle_dir="$bundle_parent/.${bundle_dir##*/}.previous.$$"
if [[ -e "$bundle_dir" ]]; then
  mv -- "$bundle_dir" "$old_bundle_dir"
  old_bundle_moved=1
fi
mv -- "$stage_dir" "$bundle_dir"
stage_dir=""
bundle_set_installed=1

old_binary="$work_dir/previous-llm-cc"
if [[ -e "$installed_binary" ]]; then
  mv -- "$installed_binary" "$old_binary"
  old_binary_moved=1
fi
mv -- "$staged_binary" "$installed_binary"
binary_installed=1

committed=1
rm -rf -- "$old_bundle_dir" "$old_binary"
echo "Installed $("$installed_binary" --version) to $installed_binary"
