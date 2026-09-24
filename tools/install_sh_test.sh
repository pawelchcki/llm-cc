#!/usr/bin/env bash
# Exercise the public curl/wget installer against a local fake release.
set -euo pipefail

installer="$TEST_SRCDIR/${1:?missing installer runfile key}"
python=python3
command -v python3 >/dev/null 2>&1 || python=python

case "$(uname -s)-$(uname -m)" in
  Linux-x86_64|Linux-amd64) platform=linux-x86_64 ;;
  Linux-aarch64|Linux-arm64) platform=linux-arm64 ;;
  Darwin-arm64) platform=macos-arm64 ;;
  Darwin-x86_64) platform=macos-x86_64 ;;
  *) echo "unsupported test host" >&2; exit 1 ;;
esac

release="$TEST_TMPDIR/release"
mkdir -p "$release"
asset="llm-cc-1.2.3-$platform"
cat > "$release/$asset" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
case "${1:-}" in
  --version) echo 'llm-cc 1.2.3' ;;
  backends)
    [[ "$2" == fetch ]]
    shift 2
    echo "$*" >> "${FETCH_LOG:?}"
    [[ "${FAIL_FETCH:-}" != 1 ]]
    ;;
  *) exit 2 ;;
esac
EOF
chmod 0755 "$release/$asset"
sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}
printf '%s  %s\n' "$(sha256 "$release/$asset")" "$asset" > "$release/$asset.sha256"
printf '{\n  "version": "1.2.3",\n  "git_sha": "%s"\n}\n' "$(printf 'a%.0s' $(seq 40))" > "$release/release-manifest.json"

port_file="$TEST_TMPDIR/port"
"$python" - "$release" "$port_file" <<'PY' &
import http.server, os, socketserver, sys
os.chdir(sys.argv[1])
class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass
with socketserver.TCPServer(("127.0.0.1", 0), Quiet) as server:
    with open(sys.argv[2], "w") as out:
        out.write(str(server.server_address[1]))
    server.serve_forever()
PY
server_pid=$!
trap 'kill "$server_pid" 2>/dev/null || true' EXIT
for _ in $(seq 100); do
  [[ -s "$port_file" ]] && break
  sleep 0.05
done
base="http://127.0.0.1:$(<"$port_file")"

export FETCH_LOG="$TEST_TMPDIR/fetch.log"
: > "$FETCH_LOG"
bin="$TEST_TMPDIR/bin dir"

# Latest install, explicit backend, PATH guidance.
output="$(LLM_CC_RELEASE_BASE="$base" sh "$installer" --bin-dir "$bin" --backend cuda)"
grep -q "Installed llm-cc 1.2.3 to $bin/llm-cc" <<<"$output"
grep -q 'export PATH=' <<<"$output"
[[ -x "$bin/llm-cc" ]]
if [[ "$platform" == linux-x86_64 ]]; then
  grep -qx "cuda --url $base/llm-cc-backend-cuda-linux-x86_64.bundle --assume-yes" "$FETCH_LOG"
else
  [[ ! -s "$FETCH_LOG" ]]
  grep -q 'bundles exist only for linux-x86_64' <<<"$output"
fi

# Pinned version and PATH silence.
output="$(PATH="$bin:$PATH" LLM_CC_RELEASE_BASE="$base" sh "$installer" --version v1.2.3 --bin-dir "$bin" --backend none)"
! grep -q 'export PATH=' <<<"$output"

# A pinned version that the release does not match is rejected.
! LLM_CC_RELEASE_BASE="$base" sh "$installer" --version 9.9.9 --bin-dir "$bin" --backend none 2>"$TEST_TMPDIR/err"
grep -q 'reports 1.2.3, expected 9.9.9' "$TEST_TMPDIR/err"

# Corrupted download must not replace the installed executable.
cp "$bin/llm-cc" "$TEST_TMPDIR/before"
echo corrupt >> "$release/$asset"
! LLM_CC_RELEASE_BASE="$base" sh "$installer" --bin-dir "$bin" --backend none 2>"$TEST_TMPDIR/err"
grep -q 'SHA-256 mismatch' "$TEST_TMPDIR/err"
cmp "$bin/llm-cc" "$TEST_TMPDIR/before"
[[ -z "$(find "$bin" -name '.llm-cc.*')" ]]
cp "$TEST_TMPDIR/before" "$release/$asset"

# Help works when the script arrives on stdin, the documented streamed form.
sh -s -- --help < "$installer" | grep -q -- '--backend NAME'

# A noexec temporary directory must not block the pre-installation probe.
noexec="$TEST_TMPDIR/noexec"
mkdir -p "$noexec"
if command -v unshare >/dev/null 2>&1 && unshare -Urm true 2>/dev/null; then
  TMPDIR="$noexec" unshare -Urm sh -c 'mount -t tmpfs -o noexec none "$0" && LLM_CC_RELEASE_BASE="$1" sh "$2" --bin-dir "$3" --backend none' \
    "$noexec" "$base" "$installer" "$TEST_TMPDIR/noexec-bin" >/dev/null
  [[ -x "$TEST_TMPDIR/noexec-bin/llm-cc" ]]
fi

# The documented one-liner installs, and fails instead of running an empty script.
if command -v curl >/dev/null 2>&1; then
  cp "$installer" "$release/install.sh"
  LLM_CC_RELEASE_BASE="$base" sh -c "$(curl -fsSL "$base/install.sh" || echo exit 1)" -- \
    --bin-dir "$TEST_TMPDIR/one-liner-bin" --backend none >/dev/null
  [[ -x "$TEST_TMPDIR/one-liner-bin/llm-cc" ]]
  if sh -c "$(curl -fsSL "$base/missing.sh" 2>/dev/null || echo exit 1)"; then
    echo "a failed download must not succeed" >&2
    exit 1
  fi
fi

# A truncated download fails to parse instead of running its first commands.
sed '/^mkdir -p "\$bin_dir"$/q' "$installer" > "$TEST_TMPDIR/truncated.sh"
grep -qx 'mkdir -p "$bin_dir"' "$TEST_TMPDIR/truncated.sh"
if LLM_CC_RELEASE_BASE="$base" sh -c "$(cat "$TEST_TMPDIR/truncated.sh"; echo exit 1)" -- \
  --bin-dir "$TEST_TMPDIR/truncated-bin" --backend none >/dev/null 2>&1; then
  echo "a truncated installer must not succeed" >&2
  exit 1
fi
[[ ! -e "$TEST_TMPDIR/truncated-bin" ]]

# HOME is required only when no destination is given.
env -u HOME sh "$installer" --help | grep -q -- '--bin-dir PATH'
env -u HOME LLM_CC_RELEASE_BASE="$base" sh "$installer" --bin-dir "$TEST_TMPDIR/homeless" --backend none >/dev/null
[[ -x "$TEST_TMPDIR/homeless/llm-cc" ]]
! env -u HOME LLM_CC_RELEASE_BASE="$base" sh "$installer" --backend none 2>"$TEST_TMPDIR/err"
grep -q 'HOME is unset' "$TEST_TMPDIR/err"

if [[ "$platform" == linux-x86_64 ]]; then
  # A failed backend fetch suggests a retry that works before PATH is updated.
  ! FAIL_FETCH=1 LLM_CC_RELEASE_BASE="$base" sh "$installer" --bin-dir "$bin" --backend rocm \
    >/dev/null 2>"$TEST_TMPDIR/err"
  grep -qF "retry later with: \"$bin/llm-cc\" backends fetch rocm --url $base/llm-cc-backend-rocm-linux-x86_64.bundle --assume-yes" \
    "$TEST_TMPDIR/err"

  # Automatic selection needs nvidia-smi to list a GPU, not merely to exist.
  fake_gpu="$TEST_TMPDIR/fake-gpu"
  mkdir -p "$fake_gpu"
  printf '#!/bin/sh\nexit "${NVIDIA_SMI_STATUS:?}"\n' > "$fake_gpu/nvidia-smi"
  chmod 0755 "$fake_gpu/nvidia-smi"
  : > "$FETCH_LOG"
  PATH="$fake_gpu:$PATH" NVIDIA_SMI_STATUS=0 LLM_CC_RELEASE_BASE="$base" sh "$installer" --bin-dir "$bin" >/dev/null
  grep -q '^cuda ' "$FETCH_LOG"
  if [[ ! -e /dev/nvidiactl ]]; then
    : > "$FETCH_LOG"
    output="$(PATH="$fake_gpu:$PATH" NVIDIA_SMI_STATUS=9 LLM_CC_RELEASE_BASE="$base" sh "$installer" --bin-dir "$bin")"
    if [[ -e /dev/kfd ]]; then
      grep -q '^rocm ' "$FETCH_LOG"
    else
      [[ ! -s "$FETCH_LOG" ]]
      grep -q 'No NVIDIA or AMD GPU detected' <<<"$output"
    fi
  fi
fi

# Bad arguments.
! sh "$installer" --backend opencl 2>"$TEST_TMPDIR/err"
grep -q -- '--backend must be' "$TEST_TMPDIR/err"

echo PASS
