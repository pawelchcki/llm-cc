#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
repo_root="$(dirname -- "$script_dir")"
version="$(tr -d '\r\n' < "$repo_root/VERSION")"
mode="${1:-sync}"

if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+([+-][0-9A-Za-z.-]+)?$ ]]; then
  echo "error: VERSION must contain a SemVer version, got '$version'" >&2
  exit 1
fi
if [[ "$mode" != "sync" && "$mode" != "--check" ]]; then
  echo "usage: tools/sync_version.sh [--check]" >&2
  exit 2
fi

rewrite_manifest() {
  awk -v version="$version" '
    $0 == "[workspace.package]" { workspace_package = 1 }
    workspace_package && /^version = "/ {
      print "version = \"" version "\""
      workspace_package = 0
      next
    }
    { print }
  ' "$repo_root/lmcc/Cargo.toml"
}

rewrite_lockfile() {
  awk -v version="$version" '
    /^name = "lmcc-(cli|core|lang)"$/ { lmcc_package = 1 }
    lmcc_package && /^version = "/ {
      print "version = \"" version "\""
      lmcc_package = 0
      next
    }
    { print }
  ' "$repo_root/lmcc/Cargo.lock"
}

rewrite_module() {
  awk -v version="$version" '
    /^module\($/ { root_module = 1 }
    root_module && /^    version = "/ {
      print "    version = \"" version "\","
      root_module = 0
      next
    }
    { print }
  ' "$repo_root/MODULE.bazel"
}

manifest_tmp="$(mktemp)"
lock_tmp="$(mktemp)"
module_tmp="$(mktemp)"
trap 'rm -f "$manifest_tmp" "$lock_tmp" "$module_tmp"' EXIT
rewrite_manifest > "$manifest_tmp"
rewrite_lockfile > "$lock_tmp"
rewrite_module > "$module_tmp"

if [[ "$mode" == "--check" ]]; then
  status=0
  cmp -s "$manifest_tmp" "$repo_root/lmcc/Cargo.toml" || status=1
  cmp -s "$lock_tmp" "$repo_root/lmcc/Cargo.lock" || status=1
  cmp -s "$module_tmp" "$repo_root/MODULE.bazel" || status=1
  if ((status != 0)); then
    echo "Package metadata is out of sync with VERSION; run tools/sync_version.sh" >&2
    exit 1
  fi
  echo "Cargo metadata matches VERSION ($version)."
  exit 0
fi

cp "$manifest_tmp" "$repo_root/lmcc/Cargo.toml"
cp "$lock_tmp" "$repo_root/lmcc/Cargo.lock"
cp "$module_tmp" "$repo_root/MODULE.bazel"
echo "Synchronized Bazel and Cargo metadata to $version."
