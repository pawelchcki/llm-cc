#!/usr/bin/env bash
set -euo pipefail
if command -v python3 >/dev/null 2>&1; then
  exec python3 "$TEST_SRCDIR/$1" "$2" "$TEST_SRCDIR/$3"
fi
exec python "$TEST_SRCDIR/$1" "$2" "$TEST_SRCDIR/$3"
