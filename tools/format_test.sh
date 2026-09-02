#!/usr/bin/env bash
set -euo pipefail

formatter="${1:?missing clang-format path}"
style="${2:?missing clang-format style path}"
shift 2
exec "$formatter" --style="file:$style" --dry-run --Werror "$@"
