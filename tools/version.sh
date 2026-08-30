#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
repo_root="$(dirname -- "$script_dir")"
raw_version="$(tr -d '[:space:]' < "$repo_root/VERSION")"

if [[ "$raw_version" =~ ^([0-9]+\.[0-9]+)(\.[0-9]+)?$ ]]; then
  base="${BASH_REMATCH[1]}"
else
  echo "error: VERSION must contain MAJOR.MINOR or MAJOR.MINOR.PATCH, got '$raw_version'" >&2
  exit 1
fi

if command -v git >/dev/null 2>&1 &&
    git -C "$repo_root" rev-parse --git-dir >/dev/null 2>&1 &&
    git -C "$repo_root" rev-parse --verify HEAD >/dev/null 2>&1; then
  if count="$(git -C "$repo_root" rev-list --count HEAD 2>/dev/null)" &&
      sha="$(git -C "$repo_root" rev-parse --short HEAD 2>/dev/null)"; then
    meta="+g$sha"
    if [[ -n "$(git -C "$repo_root" status --porcelain 2>/dev/null || true)" ]]; then
      meta="$meta.dirty"
    fi
    printf '%s\n' "$base.$count$meta"
    exit 0
  fi
fi

printf '%s\n' "$base.0+unknown"
