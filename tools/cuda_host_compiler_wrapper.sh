#!/bin/bash
set -euo pipefail

# NVCC delegates its host phase to pinned Clang. NVIDIA's CUDA x86 headers
# explicitly reject libc++, so this one frontend uses the compact bundle's
# pinned libstdc++ headers/runtime in addition to the portable glibc sysroot. No
# GCC executable participates in the build.
script_path="${BASH_SOURCE[0]}"
if [[ "${script_path}" != /* ]]; then
  script_path="${PWD}/${script_path}"
fi
script_dir="${script_path%/*}"
exec_root="${script_dir%/tools}"
clang_root="${exec_root}/external/toolchains_llvm++llvm+llvm_toolchain_llvm"
clangxx="${clang_root}/bin/clang++"
sysroot_root="${exec_root}/external/+http_archive+linux_glibc_sysroot"
gcc_root="${exec_root}/external/+http_archive+cuda_host_toolchain"

is_link=1
skip_next=0
filtered=()
for arg in "$@"; do
  if (( skip_next )); then
    skip_next=0
    continue
  fi
  case "${arg}" in
    -c|-E|-S)
      is_link=0
      filtered+=("${arg}")
      ;;
    -resource-dir|-cxx-isystem|-idirafter)
      skip_next=1
      ;;
    --target=*|--sysroot=*|-rtlib=*|-stdlib=*|-fuse-ld=lld|-l:libc++.a|-l:libc++abi.a|-l:libunwind.a|-Bexternal/toolchains_llvm*|-fcolor-diagnostics|-Wthread-safety|-Wself-assign|-Xclang|-fno-cxx-modules|-Wno-module-import-in-extern-c|-nostdinc++|-fno-canonical-system-headers)
      ;;
    *)
      filtered+=("${arg}")
      ;;
  esac
done

filtered+=(
  "--gcc-toolchain=${gcc_root}"
  "--sysroot=${sysroot_root}"
  "-nostdinc++"
  "-w"
  "-include" "${exec_root}/tools/cuda_glibc_compat.h"
  "-isystem" "${gcc_root}/x86_64-buildroot-linux-gnu/include/c++/12.3.0"
  "-isystem" "${gcc_root}/x86_64-buildroot-linux-gnu/include/c++/12.3.0/x86_64-buildroot-linux-gnu"
  "-isystem" "${gcc_root}/x86_64-buildroot-linux-gnu/include/c++/12.3.0/backward"
)

if (( is_link )); then
  filtered+=(
    "-static-libstdc++"
    "-static-libgcc"
  )
fi
exec "${clangxx}" "${filtered[@]}"
