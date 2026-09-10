#!/usr/bin/env bash
# Run as root inside a clean Ubuntu container, with this checkout in /workspace.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends ca-certificates libxml2 libstdc++6 zlib1g python3 git perl xz-utils bzip2 unzip binutils
if command -v make; then
  echo 'This regression requires a container without Make' >&2
  exit 1
fi
cd /workspace
# /build is a writable copy: the input checkout may be mounted read-only.
mkdir -p /build/source
tar --exclude=.git --exclude=.codex --exclude=.agents --exclude='bazel-*' -cf - . | tar -xf - -C /build/source
cd /build/source
# Exclude host Bazel convenience symlinks if this is a development checkout.
for link in bazel-*; do
  if [[ -L "$link" ]]; then rm "$link"; fi
done
backend="${BACKEND:-cpu}"
bazel --nohome_rc --output_user_root=/build/bazel run --config=release --config="$backend" \
  --jobs="${JOBS:-16}" --repository_cache=/repository-cache \
  --//:source_version=0.2.0-container-fixture \
  --//:source_commit=a300b37b912a06f79869582b7f870eace005b329 \
  //:install -- --prefix /opt/llm-cc
# Reinstall with a restrictive process umask.
# The normal installer copies assets with explicit portable modes.
umask 077
bazel --nohome_rc --output_user_root=/build/bazel run --config=release --config="$backend" \
  --jobs="${JOBS:-16}" --repository_cache=/repository-cache \
  --//:source_version=0.2.0-container-fixture \
  --//:source_commit=a300b37b912a06f79869582b7f870eace005b329 \
  //:install -- --prefix /opt/restrictive/llm-cc
python3 - "$backend" <<'PY'
import os
from pathlib import Path
import subprocess
import sys
os.setgid(10001)
os.setuid(10001)
root = Path('/opt/restrictive/llm-cc')
for path in [root, *root.rglob('*')]:
    if path.name == '.llm-cc-install.lock':
        continue
    assert os.access(path, os.R_OK | (os.X_OK if path.is_dir() else 0)), path
    if path.is_file() and path.name != '.llm-cc-install.lock':
        with path.open('rb') as stream:
            stream.read(1)
binary = str(root / 'bin/llm-cc')
subprocess.run([binary, '--version'], check=True)
if sys.argv[1] == 'cuda':
    env = dict(os.environ, LLM_CC_RUNTIME_DIR=str(root / 'lib/llm-cc'))
    subprocess.run([binary, 'backends', 'fetch', 'cuda', '--no-download'], env=env, check=True)
if Path('/model.gguf').exists():
    subprocess.run([binary, 'score', '--model', '/model.gguf', '--file', '/workspace/testdata/lang/functions.cc', '--force-cpu', '--no-download', '--progress', 'never'], check=True, env=dict(os.environ, HOME='/tmp'))
PY
readelf -d /opt/llm-cc/bin/llm-cc
bash tools/check_glibc_version.sh /opt/llm-cc/bin/llm-cc 2.28
