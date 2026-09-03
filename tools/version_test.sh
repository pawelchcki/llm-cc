#!/usr/bin/env bash
set -euo pipefail

if ! command -v git >/dev/null 2>&1; then
  echo "SKIP: git is unavailable"
  exit 0
fi

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
version_script="$script_dir/version.sh"
status_script="$script_dir/bazel_status.sh"
test_tmp="$(mktemp -d)"
trap 'rm -rf -- "$test_tmp"' EXIT

export GIT_CONFIG_GLOBAL=/dev/null
export GIT_CONFIG_NOSYSTEM=1
export GIT_AUTHOR_NAME="llm-cc version test"
export GIT_AUTHOR_EMAIL="version-test@example.invalid"
export GIT_COMMITTER_NAME="$GIT_AUTHOR_NAME"
export GIT_COMMITTER_EMAIL="$GIT_AUTHOR_EMAIL"
export GIT_AUTHOR_DATE="2000-01-01T00:00:00Z"
export GIT_COMMITTER_DATE="$GIT_AUTHOR_DATE"
export LC_ALL=C
export TZ=UTC

make_repo() {
  local name="$1"
  local version="$2"
  local repo="$test_tmp/$name"

  mkdir -p "$repo/tools"
  cp "$version_script" "$repo/tools/version.sh"
  cp "$status_script" "$repo/tools/bazel_status.sh"
  printf '%s\n' "$version" > "$repo/VERSION"
  printf 'tracked\n' > "$repo/tracked.txt"
  git -C "$repo" init -q
  git -C "$repo" add VERSION tools/bazel_status.sh tools/version.sh tracked.txt
  git -C "$repo" commit -q -m initial
  printf '%s\n' "$repo"
}

tagged_repo="$(make_repo tagged 0.1.0)"
git -C "$tagged_repo" tag v0.1.0
tagged_output="$("$tagged_repo/tools/version.sh")"
[[ "$tagged_output" == "0.1.0" ]] || {
  echo "exact tag: expected 0.1.0, got $tagged_output" >&2
  exit 1
}

untagged_repo="$(make_repo untagged 0.1.0)"
untagged_sha="$(git -C "$untagged_repo" rev-parse --short HEAD)"
untagged_output="$("$untagged_repo/tools/version.sh")"
[[ "$untagged_output" == "0.1.0-dev.1+g$untagged_sha" ]] || {
  echo "untagged: expected 0.1.0-dev.1+g$untagged_sha, got $untagged_output" >&2
  exit 1
}
untagged_full_sha="$(git -C "$untagged_repo" rev-parse HEAD)"
untagged_status="$("$untagged_repo/tools/bazel_status.sh")"
grep -qx "STABLE_LLM_CC_GIT_SHA $untagged_full_sha" <<<"$untagged_status" || {
  echo "clean status did not stamp HEAD" >&2
  exit 1
}
grep -qx "STABLE_LLM_CC_ARTIFACT_BASE_URL https://ci-artifacts.pawelchcki.workers.dev/pawelchcki/llm-cc/$untagged_full_sha" <<<"$untagged_status" || {
  echo "clean status did not stamp the resolver URL" >&2
  exit 1
}

printf 'dirty\n' >> "$untagged_repo/tracked.txt"
dirty_output="$("$untagged_repo/tools/version.sh")"
[[ "$dirty_output" == "0.1.0-dev.1+g$untagged_sha.dirty" ]] || {
  echo "dirty tree: expected .dirty suffix, got $dirty_output" >&2
  exit 1
}
dirty_status="$("$untagged_repo/tools/bazel_status.sh")"
grep -qx 'STABLE_LLM_CC_GIT_SHA unknown' <<<"$dirty_status" || {
  echo "dirty status stamped a clean commit" >&2
  exit 1
}
grep -qx 'STABLE_LLM_CC_ARTIFACT_BASE_URL none' <<<"$dirty_status" || {
  echo "dirty status enabled backend fetching" >&2
  exit 1
}

invalid_repo="$(make_repo invalid 0.1)"
if "$invalid_repo/tools/version.sh" > "$test_tmp/invalid.out" 2> "$test_tmp/invalid.err"; then
  echo "VERSION without a patch component unexpectedly succeeded" >&2
  exit 1
fi
if ! grep -q 'exactly MAJOR.MINOR.PATCH' "$test_tmp/invalid.err"; then
  echo "invalid VERSION failure did not explain the required format" >&2
  exit 1
fi

mismatch_repo="$(make_repo mismatch 0.1.0)"
git -C "$mismatch_repo" tag v9.9.9
if "$mismatch_repo/tools/version.sh" > "$test_tmp/mismatch.out" 2> "$test_tmp/mismatch.err"; then
  echo "mismatched HEAD tag unexpectedly succeeded" >&2
  exit 1
fi
if ! grep -q "HEAD tag 'v9.9.9' and VERSION '0.1.0' disagree" "$test_tmp/mismatch.err"; then
  echo "mismatched tag failure did not explain the disagreement" >&2
  exit 1
fi

echo "version tests passed"
