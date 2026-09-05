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
mkdir -p "$bin_dir"
physical_bin_dir="$(cd -P -- "$bin_dir" && pwd)"
install_prefix="$(dirname -- "$physical_bin_dir")"
install_root="$install_prefix/lib/llm-cc"

# Serialize changes to the executable and its sibling backend root. flock is
# preferred because the kernel releases it even after an uncatchable signal;
# macOS shlock records the owner PID and reclaims abandoned lock files.
lock_path="$install_prefix/.llm-cc-install.lock"
lock_owned=0
release_lock() {
  if ((lock_owned)); then
    rm -f -- "$lock_path"
    lock_owned=0
  fi
}
trap release_lock EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
if command -v flock >/dev/null 2>&1; then
  exec 9>"$lock_path"
  flock 9
elif command -v shlock >/dev/null 2>&1; then
  until shlock -f "$lock_path" -p "$$"; do
    sleep 0.1
  done
  lock_owned=1
else
  echo "error: installing requires flock or shlock for transaction locking" >&2
  exit 1
fi

mkdir -p "$install_root"
work_dir="$(mktemp -d "$physical_bin_dir/.llm-cc-install.XXXXXXXX")"
stage_dir=""
staged_bundle_dir=""
staged_binary=""
bundle_dir=""
old_bundle_dir=""
installed_binary="$physical_bin_dir/llm-cc"
old_binary=""
committed=0
old_bundle_moved=0
bundle_set_installed=0
old_binary_moved=0
binary_installed=0
rollback() {
  # The old executable and bundle set remain recoverable until both swaps have
  # completed. Infer successful moves from the filesystem so an interruption
  # between a move and its bookkeeping assignment remains recoverable.
  if [[ -n "$staged_binary" && ! -e "$staged_binary" &&
        -e "$installed_binary" ]]; then
    binary_installed=1
  fi
  if [[ -n "$old_binary" && -e "$old_binary" ]]; then
    old_binary_moved=1
  fi
  if [[ -n "$staged_bundle_dir" && ! -e "$staged_bundle_dir" &&
        -e "$bundle_dir" ]]; then
    bundle_set_installed=1
  fi
  if [[ -n "$old_bundle_dir" && -e "$old_bundle_dir" ]]; then
    old_bundle_moved=1
  fi
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
  trap - EXIT INT TERM
  if ((status != 0 && committed == 0)); then
    if ! rollback; then
      echo "error: installation rollback failed; recover from $work_dir or $old_bundle_dir" >&2
      release_lock
      exit "$status"
    fi
  fi
  [[ -z "$stage_dir" ]] || rm -rf -- "$stage_dir"
  rm -rf -- "$work_dir"
  release_lock
  exit "$status"
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

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
staged_bundle_dir="$stage_dir"

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
