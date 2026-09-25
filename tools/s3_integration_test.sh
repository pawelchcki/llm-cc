#!/usr/bin/env bash
# Signs real requests through libcurl against a local SigV4-verifying server.
set -euo pipefail
if command -v python3 >/dev/null 2>&1; then
  exec python3 "$TEST_SRCDIR/$1" "$2"
fi
exec python "$TEST_SRCDIR/$1" "$2"
