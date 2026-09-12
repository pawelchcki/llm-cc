#!/usr/bin/env bash
# Bazel supplies the repository-qualified key, including renamed dependencies.
set -euo pipefail
entry="${1:?missing entry runfile key}"
stage="${2:?missing comparison stage}"
shift 2
runfiles="${RUNFILES_DIR:-${TEST_SRCDIR:-$0.runfiles}}"
if [[ -f "$runfiles/$entry" ]]; then
  entry="$runfiles/$entry"
elif [[ -n "${RUNFILES_MANIFEST_FILE:-}" ]]; then
  entry="$(python3 - "$RUNFILES_MANIFEST_FILE" "$entry" <<'PY'
import sys
for line in open(sys.argv[1], encoding="utf-8"):
    escaped = line.startswith(" ")
    logical, physical = line.rstrip("\n").lstrip(" ").split(" ", 1)
    if escaped:
        def decode(value):
            return value.replace("\\s", " ").replace("\\n", "\n").replace("\\b", "\\")
        logical, physical = decode(logical), decode(physical)
    if logical == sys.argv[2]:
        print(physical)
        break
else:
    raise SystemExit("Missing comparison entry in runfiles manifest")
PY
)"
else
  echo "Cannot resolve comparison runfiles: $entry" >&2
  exit 1
fi
package="$(cd -- "$(dirname -- "$entry")" && pwd)"
root="$(dirname -- "$(dirname -- "$package")")"
export PYTHONPATH="$root${PYTHONPATH:+:$PYTHONPATH}"
export PYTHONDONTWRITEBYTECODE=1
if [[ "$stage" == test ]]; then
  exec python3 -m unittest discover -s "$package" -t "$root" -p 'test_*.py' "$@"
fi
# bazel run starts inside runfiles; explicit paths should resolve in the caller.
if [[ -n "${BUILD_WORKING_DIRECTORY:-}" ]]; then
  cd -- "$BUILD_WORKING_DIRECTORY"
fi
exec python3 -m tools.comparison "$stage" "$@"
