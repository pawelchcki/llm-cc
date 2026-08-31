#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
printf 'STABLE_LLM_CC_VERSION %s\n' "$("$script_dir/version.sh")"
