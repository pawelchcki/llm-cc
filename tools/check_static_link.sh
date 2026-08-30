#!/usr/bin/env bash
# Fails if the release CPU binary links anything beyond the loader and the
# libc family (see DESIGN.md "Static linking").
set -euo pipefail

binary="${1:?usage: check_static_link.sh <binary>}"
allowed='^(linux-vdso|libc|libm|libpthread|libdl|librt|ld-linux)'

violations=$(ldd "$binary" | awk '{print $1}' | sed 's/.*\///' \
  | grep -Ev "$allowed" || true)
if [[ -n "$violations" ]]; then
  echo "disallowed dynamic dependencies in $binary:" >&2
  echo "$violations" >&2
  exit 1
fi
echo "$binary links only allowed system libraries"
