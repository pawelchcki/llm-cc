#!/usr/bin/env bash
# A branch dispatch rehearses the exact-tag path using a runner-local tag.
set -euo pipefail
version="$(tr -d '\r\n' < version.txt)"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]
tag="v$version"
if [[ "${GITHUB_REF:-}" == refs/tags/* ]]; then
  [[ "$GITHUB_REF" == "refs/tags/$tag" ]] || {
    echo "Tag and version.txt disagree" >&2
    exit 1
  }
else
  [[ "${RELEASE_PUBLISH:-false}" == false ]] || {
    echo "Publishing requires dispatching a version tag" >&2
    exit 1
  }
  git tag --force "$tag" HEAD
fi
[[ -z "$(git status --porcelain)" ]] || {
  echo "Release builds require a clean checkout" >&2
  exit 1
}
status="$(tools/bazel_status.sh)"
grep -qx "STABLE_LLM_CC_VERSION $version" <<<"$status"
grep -qx "STABLE_LLM_CC_ARTIFACT_BASE_URL https://github.com/pawelchcki/llm-cc/releases/download/$tag" <<<"$status"
printf 'version=%s\ntag=%s\ncommit=%s\n' "$version" "$tag" "$(git rev-parse HEAD)" >> "$GITHUB_OUTPUT"
