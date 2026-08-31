#!/bin/bash
set -euo pipefail

# NVCC needs GNU libstdc++ on x86_64, while rules_foreign_cc contributes the
# registered Clang toolchain's flags to CUDA link commands. Keep the pinned GCC
# for both phases and remove only flags that are Clang-driver-specific. Static
# GNU runtimes keep the resulting CUDA backend's runtime closure self-contained.
script_path="${BASH_SOURCE[0]}"
if [[ "${script_path}" != /* ]]; then
  script_path="${PWD}/${script_path}"
fi
script_dir="${script_path%/*}"
exec_root="${script_dir%/tools}"
gxx="${exec_root}/external/+http_archive+cuda_host_toolchain/bin/x86_64-buildroot-linux-gnu-g++"

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
    -resource-dir)
      skip_next=1
      ;;
    --target=*|--sysroot=*|-rtlib=*|-stdlib=*|-fuse-ld=lld|-l:libc++.a|-l:libc++abi.a|-l:libunwind.a)
      ;;
    *)
      filtered+=("${arg}")
      ;;
  esac
done

if (( is_link )); then
  filtered+=("-static-libstdc++" "-static-libgcc")
fi
exec "${gxx}" "${filtered[@]}"
