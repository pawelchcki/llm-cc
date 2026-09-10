#!/usr/bin/env bash
# Analysis-only architecture/provenance regressions, run from the checkout.
set -euo pipefail
result_dir="$(mktemp -d "${TMPDIR:-/tmp}/llm-cc-config.XXXXXXXX")"
trap 'rm -rf "$result_dir"' EXIT
args=(--config=release --config=cuda)
for value in '' 'compute_86;;compute_80' 'compute_86:sm_invalid' 'compute_86:'; do
  if bazel build "${args[@]}" --//:cuda_archs="$value" //:version_header > "$result_dir/invalid.log" 2>&1; then
    echo "invalid architecture selection was accepted: $value" >&2
    exit 1
  fi
done
bazel build "${args[@]}" --//:cuda_archs=compute_86:sm_86 //:version_header
cp bazel-bin/generated/version.h "$result_dir/a10.h"
bazel build "${args[@]}" '--//:cuda_archs= sm_86 ; compute_86: sm_86 ' //:version_header
cmp bazel-bin/generated/version.h "$result_dir/a10.h"
bazel build "${args[@]}" //:version_header
if cmp -s bazel-bin/generated/version.h "$result_dir/a10.h"; then
  echo 'portable and A10 configurations must differ' >&2
  exit 1
fi
bazel aquery "${args[@]}" --output=jsonproto 'mnemonic("CudaCompile", deps(//:install))' > "$result_dir/portable.json"
bazel aquery "${args[@]}" --//:cuda_archs=compute_86:sm_86 --output=jsonproto 'mnemonic("CudaCompile", deps(//:install))' > "$result_dir/a10.json"
python3 - "$result_dir" <<'PY'
import json
from pathlib import Path
import sys
root = Path(sys.argv[1])
for name, expected in [('portable', 6), ('a10', 1)]:
    graph = json.loads((root / (name + '.json')).read_text())
    actions = graph['actions']
    # aquery includes both PIC and non-PIC declared actions; the build selects 142.
    assert len(actions) == 284, len(actions)
    for action in actions:
        args = action['arguments']
        codes = [arg for arg in args if 'arch=compute_' in arg]
        assert len(codes) == expected, (name, codes)
        assert any('cuda_host_compiler.sh' in arg for arg in args), args
        if name == 'a10':
            assert all('compute_86' in code and 'sm_86' in code for code in codes), codes
print('Architecture validation, normalization, fingerprints and CUDA arguments passed')
PY
