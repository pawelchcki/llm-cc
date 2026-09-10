#!/usr/bin/env bash
set -euo pipefail
if command -v python3 >/dev/null 2>&1; then
  exec python3 "$1" "$2" "$3"
fi
exec python "$1" "$2" "$3"
