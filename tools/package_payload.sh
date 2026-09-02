#!/usr/bin/env bash
set -euo pipefail

output=""
root=""
require_static=0
declare -a sources=()
declare -a destinations=()
while (($#)); do
  case "$1" in
    --output)
      output="${2:?missing output path}"
      shift 2
      ;;
    --root)
      root="${2:?missing archive root}"
      shift 2
      ;;
    --file)
      mapping="${2:?missing file mapping}"
      sources+=("${mapping%%=*}")
      destinations+=("${mapping#*=}")
      shift 2
      ;;
    --require-static)
      require_static=1
      shift
      ;;
    *)
      echo "error: unknown package argument: $1" >&2
      exit 2
      ;;
  esac
done

if ((require_static)) && readelf -d "${sources[0]}" 2>&1 | \
    grep -q 'Dynamic section'; then
  echo "error: static fallback has an ELF dynamic section" >&2
  exit 1
fi

[[ -n "$output" && -n "$root" ]] || {
  echo "error: --output and --root are required" >&2
  exit 2
}
staging="$(mktemp -d "${TMPDIR:-/tmp}/llm-cc-package.XXXXXXXX")"
trap 'rm -rf -- "$staging"' EXIT
mkdir -p "$staging/$root"
for ((index = 0; index < ${#sources[@]}; ++index)); do
  destination="$staging/$root/${destinations[index]}"
  mkdir -p "$(dirname -- "$destination")"
  cp -L -- "${sources[index]}" "$destination"
  case "${destinations[index]}" in
    llm-cc) chmod 0755 "$destination" ;;
    *) chmod 0644 "$destination" ;;
  esac
done

LC_ALL=C tar --sort=name --format=gnu --mtime=@0 --owner=0 --group=0 \
  --numeric-owner -C "$staging" -czf "$output" "$root"
