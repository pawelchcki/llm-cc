#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
repo_root="$(dirname -- "$script_dir")"
raw_version="$(<"$repo_root/VERSION")"
version="$("$script_dir/version.sh")"

git_sha="unknown"
artifact_base_url="none"
resolved_git_sha=""
worktree_status=""
default_resolver_base="https://ci-artifacts.pawelchcki.workers.dev"

if command -v git >/dev/null 2>&1 &&
    git -C "$repo_root" rev-parse --git-dir >/dev/null 2>&1 &&
    resolved_git_sha="$(git -C "$repo_root" rev-parse HEAD 2>/dev/null)" &&
    worktree_status="$(git -C "$repo_root" status --porcelain 2>/dev/null)" &&
    [[ -z "$worktree_status" ]]; then
  git_sha="$resolved_git_sha"
  if head_tag="$(git -C "$repo_root" describe --tags --exact-match --match 'v*' HEAD 2>/dev/null)" &&
      [[ "$head_tag" == "v$raw_version" && "$version" == "$raw_version" ]]; then
    artifact_base_url="https://github.com/pawelchcki/llm-cc/releases/download/v$raw_version"
  else
    resolver_base="${LLM_CC_RESOLVER_BASE:-$default_resolver_base}"
    resolver_base="${resolver_base%/}"
    artifact_base_url="$resolver_base/pawelchcki/llm-cc/$git_sha"
  fi
fi

safe_url_pattern='^[0-9A-Za-z:/._~-]+$'
if [[ ! "$artifact_base_url" =~ $safe_url_pattern ]]; then
  echo "error: LLM_CC_RESOLVER_BASE produced an invalid artifact base URL" >&2
  exit 1
fi

printf 'STABLE_LLM_CC_VERSION %s\n' "$version"
printf 'STABLE_LLM_CC_GIT_SHA %s\n' "$git_sha"
# Bazel workspace-status values cannot be empty; "none" means fetching is disabled.
printf 'STABLE_LLM_CC_ARTIFACT_BASE_URL %s\n' "$artifact_base_url"
