#!/usr/bin/env bash
set -euo pipefail

script="$TEST_SRCDIR/$TEST_WORKSPACE/tools/install.sh"
root="$TEST_TMPDIR/prefix with spaces"
source_binary="$TEST_TMPDIR/source-llm-cc"
cuda_bundle="$TEST_TMPDIR/llm-cc-backend-cuda-linux-x86_64.bundle"
rocm_bundle="$TEST_TMPDIR/llm-cc-backend-rocm-linux-x86_64.bundle"

cat > "$source_binary" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
case "${1:-}" in
  --version) echo 'llm-cc test' ;;
  backends)
    [[ "${2:-}" == path ]]
    key="${BUILD_KEY:-test-build}"
    case "${3:-cuda}" in
      cuda) echo "$LLM_CC_RUNTIME_DIR/backends/$key/cuda.bundle" ;;
      rocm)
        if [[ "${FAIL_ROCM:-}" == 1 ]]; then
          exit 1
        fi
        echo "$LLM_CC_RUNTIME_DIR/backends/$key/rocm.bundle"
        ;;
    esac
    ;;
  *) exit 2 ;;
esac
EOF
chmod +x "$source_binary"
for file in "$cuda_bundle" "$rocm_bundle"; do
  printf 'bundle' > "$file"
  printf 'checksum' > "$file.sha256"
done
printf '{"git_sha":"test"}' > "$TEST_TMPDIR/cuda.manifest.json"
printf '{"git_sha":"test"}' > "$TEST_TMPDIR/rocm.manifest.json"

BUILD_KEY=first-build bash "$script" "$source_binary" \
  --bundle "$cuda_bundle" --checksum "$cuda_bundle.sha256" \
  --manifest "$TEST_TMPDIR/cuda.manifest.json" -- --prefix "$root"
bundle_dir="$root/lib/llm-cc/backends/first-build"
[[ -x "$root/bin/llm-cc" && -f "$bundle_dir/cuda.bundle" ]]

# A failure before the full set is staged leaves the active installation.
if BUILD_KEY=first-build FAIL_ROCM=1 bash "$script" "$source_binary" \
  --bundle "$rocm_bundle" --checksum "$rocm_bundle.sha256" \
  --manifest "$TEST_TMPDIR/rocm.manifest.json" -- --prefix "$root"; then
  echo 'expected backend staging failure' >&2
  exit 1
fi
[[ -x "$root/bin/llm-cc" && -f "$bundle_dir/cuda.bundle" ]]

# A same-build backend change replaces the complete selected set.
BUILD_KEY=first-build bash "$script" "$source_binary" \
  --bundle "$rocm_bundle" --checksum "$rocm_bundle.sha256" \
  --manifest "$TEST_TMPDIR/rocm.manifest.json" -- --prefix "$root"
[[ -f "$bundle_dir/rocm.bundle" && ! -e "$bundle_dir/cuda.bundle" ]]

# Fail the move into bin after the old executable was backed up. This exercises
# the transaction's last rollback boundary without a production-only hook.
mv_wrapper_dir="$TEST_TMPDIR/mv-wrapper"
mkdir "$mv_wrapper_dir"
cat > "$mv_wrapper_dir/mv" <<'EOF'
#!/usr/bin/env bash
if [[ "${FAIL_BINARY_MOVE:-}" == 1 && "${!#}" == */bin/llm-cc &&
      ! -e "${MV_WRAPPER_MARKER:?}" ]]; then
  : > "$MV_WRAPPER_MARKER"
  exit 1
fi
exec /bin/mv "$@"
EOF
chmod +x "$mv_wrapper_dir/mv"
if BUILD_KEY=first-build FAIL_BINARY_MOVE=1 \
  MV_WRAPPER_MARKER="$TEST_TMPDIR/mv-failed" PATH="$mv_wrapper_dir:$PATH" \
  bash "$script" "$source_binary" -- --prefix "$root"; then
  echo 'expected post-backup rollback failure' >&2
  exit 1
fi
[[ -x "$root/bin/llm-cc" && -f "$bundle_dir/rocm.bundle" ]]

# A failed first install leaves neither a new executable nor a new bundle set.
fresh_root="$TEST_TMPDIR/fresh prefix"
rm -f "$TEST_TMPDIR/mv-failed"
if BUILD_KEY=fresh-build FAIL_BINARY_MOVE=1 \
  MV_WRAPPER_MARKER="$TEST_TMPDIR/mv-failed" PATH="$mv_wrapper_dir:$PATH" \
  bash "$script" "$source_binary" \
    --bundle "$cuda_bundle" --checksum "$cuda_bundle.sha256" \
    --manifest "$TEST_TMPDIR/cuda.manifest.json" -- --prefix "$fresh_root"; then
  echo 'expected fresh-install rollback failure' >&2
  exit 1
fi
[[ ! -e "$fresh_root/bin/llm-cc" &&
   ! -e "$fresh_root/lib/llm-cc/backends/fresh-build/cuda.bundle" ]]

# A CPU reinstall swaps the matching build-key set with an empty directory.
BUILD_KEY=first-build bash "$script" "$source_binary" -- --prefix "$root"
[[ -x "$root/bin/llm-cc" && ! -e "$bundle_dir/rocm.bundle" ]]

# A different build key does not alter a prior key's installed bundle set.
BUILD_KEY=other-build bash "$script" "$source_binary" \
  --bundle "$cuda_bundle" --checksum "$cuda_bundle.sha256" \
  --manifest "$TEST_TMPDIR/cuda.manifest.json" -- --prefix "$root"
BUILD_KEY=third-build bash "$script" "$source_binary" -- --prefix "$root"
[[ -f "$root/lib/llm-cc/backends/other-build/cuda.bundle" ]]
