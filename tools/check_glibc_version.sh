#!/usr/bin/env bash
# Fails when an ELF binary requires a glibc symbol newer than the requested
# compatibility floor.
set -euo pipefail

binary="${1:?usage: check_glibc_version.sh <binary> [max-version, default 2.37]}"
max_version="${2:-2.37}"

if [[ ! -f "$binary" ]]; then
  echo "binary not found: $binary" >&2
  exit 1
fi
if [[ ! "$max_version" =~ ^[0-9]+([.][0-9]+)*$ ]]; then
  echo "invalid maximum glibc version: $max_version" >&2
  exit 1
fi

versions=$(objdump -T "$binary" \
  | sed -nE 's/.*GLIBC_([0-9]+([.][0-9]+)*).*/\1/p' \
  | sort -Vu)

violations=""
while IFS= read -r version; do
  [[ -z "$version" ]] && continue
  if [[ "$(printf '%s\n%s\n' "$max_version" "$version" | sort -V | tail -n 1)" != "$max_version" ]]; then
    violations+="${violations:+$'\n'}$version"
  fi
done <<<"$versions"

if [[ -n "$violations" ]]; then
  echo "$binary requires glibc symbol versions newer than $max_version:" >&2
  echo "$violations" >&2
  exit 1
fi

highest=$(printf '%s\n' "$versions" | tail -n 1)
echo "$binary highest referenced glibc symbol version: ${highest:-none} (maximum allowed: $max_version)"
