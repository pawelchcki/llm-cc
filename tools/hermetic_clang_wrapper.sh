#!/bin/bash
set -euo pipefail

# ROCm Clang resolves its resource directory through Bazel's repository-cache
# symlink. Make toolchain roots absolute as CMake does, so include_next sees a
# single consistent path namespace inside the sandbox.
compiler="$1"
shift
if [[ "${compiler}" != /* ]]; then
  compiler="${PWD}/${compiler}"
fi

args=()
path_arg=0
for arg in "$@"; do
  if (( path_arg )); then
    if [[ "${arg}" != /* ]]; then
      arg="${PWD}/${arg}"
    fi
    path_arg=0
    args+=("${arg}")
    continue
  fi
  case "${arg}" in
    -I|-isystem|-idirafter|-resource-dir|-L)
      args+=("${arg}")
      path_arg=1
      ;;
    --rocm-path=*|--rocm-device-lib-path=*|--gcc-toolchain=*|--sysroot=*)
      option="${arg%%=*}"
      path="${arg#*=}"
      if [[ "${path}" != /* ]]; then
        path="${PWD}/${path}"
      fi
      args+=("${option}=${path}")
      ;;
    -L*)
      path="${arg#-L}"
      if [[ "${path}" != /* ]]; then
        path="${PWD}/${path}"
      fi
      args+=("-L${path}")
      ;;
    -Wl,--version-script=*)
      path="${arg#-Wl,--version-script=}"
      if [[ "${path}" != /* ]]; then
        path="${PWD}/${path}"
      fi
      args+=("-Wl,--version-script=${path}")
      ;;
    *)
      args+=("${arg}")
      ;;
  esac
done

exec "${compiler}" "${args[@]}"
