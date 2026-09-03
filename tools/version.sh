#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
repo_root="$(dirname -- "$script_dir")"
raw_version="$(<"$repo_root/VERSION")"

if [[ ! "$raw_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "error: VERSION must contain exactly MAJOR.MINOR.PATCH, got '$raw_version'" >&2
  exit 1
fi

if command -v git >/dev/null 2>&1 &&
    git -C "$repo_root" rev-parse --git-dir >/dev/null 2>&1 &&
    git -C "$repo_root" rev-parse --verify HEAD >/dev/null 2>&1; then
  dirty=""
  if [[ -n "$(git -C "$repo_root" status --porcelain 2>/dev/null || true)" ]]; then
    dirty=".dirty"
  fi

  if head_tag="$(git -C "$repo_root" describe --tags --exact-match --match 'v*' HEAD 2>/dev/null)"; then
    if [[ "$head_tag" != "v$raw_version" ]]; then
      echo "error: HEAD tag '$head_tag' and VERSION '$raw_version' disagree" >&2
      exit 1
    fi
    if [[ -z "$dirty" ]]; then
      printf '%s\n' "$raw_version"
      exit 0
    fi
  fi

  if count="$(git -C "$repo_root" rev-list --count HEAD 2>/dev/null)" &&
      sha="$(git -C "$repo_root" rev-parse --short HEAD 2>/dev/null)"; then
    printf '%s-dev.%s+g%s%s\n' "$raw_version" "$count" "$sha" "$dirty"
    exit 0
  fi
fi

printf '%s-dev.0+unknown\n' "$raw_version"
