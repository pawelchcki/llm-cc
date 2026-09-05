#!/usr/bin/env python3
"""Export an immutable, intentionally small dd-trace-c comparison corpus."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

BASE = "11133357"
AFTER = "a417106c"
PAIRS = ["test_go_pclntab.c", "test_go_hook_engine.c", "test_go_ctx.c", "test_go_read.c"]
HELDOUT = [
    "libdd_go_hook/test/test_go_abi.c",
    "libdd_go_hook/test/test_go_arm64_decode.c",
    "libdd_go_hook/test/test_go_type.c",
    "libdd_go_hook/test/test_go_x86_decode.c",
    "libdd_go_hook/src/go_abi.c",
    "libdd_go_hook/src/go_ctx.c",
    "libdd_go_hook/src/go_read.c",
    "libdd_go_hook/src/go_type.c",
    "libdd_go_hook/src/go_x86_decode.c",
    "libdd_go_hook/src/go_pclntab.c",
    "libdd_go_hook/test/fixtures/hello_http.go",
    "libdd_go_hook/test/fixtures/hello_parent_child.go",
    "libdd_nccl_profiler/test/test_inspector_event_pool.cc",
    "libdd_nccl_profiler/test/test_inspector_dd_span_accessor.cc",
    "libdd_nccl_profiler/test/nccl_allreduce_workload.py",
    "libdd_nccl_profiler/test/analyze_spans.py",
    "bench/bench_python_format.c",
    "bench/bench_python_stack_capture.py",
]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repository", type=Path)
    parser.add_argument("--output", type=Path, default=Path(__file__).parent)
    args = parser.parse_args()
    def git(*command):
        return subprocess.check_output(["git", "-C", str(args.repository), *command])
    base = git("rev-parse", BASE).decode().strip()
    after = git("rev-parse", AFTER).decode().strip()
    corpus = args.output / "corpus"
    corpus.mkdir(parents=True, exist_ok=True)
    records = []
    exports = [("heldout", path, after) for path in HELDOUT]
    for name in PAIRS:
        exports += [(version, "libdd_go_hook/test/" + name, commit)
                    for version, commit in [("before", base), ("after", after)]]
    for group, path, commit in exports:
        data = git("show", commit + ":" + path)
        relative = group + "/" + path
        target = corpus / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        records.append(dict(id=relative, group=group, source=path, commit=commit,
                            bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
    # A repository root makes llm-cc's entropy cache available for tau sweeps.
    subprocess.run(["git", "init", "-q", str(corpus)], check=True)
    manifest = dict(base=base, after=after, files=records)
    (args.output / "corpus.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Exported {len(records)} files, {sum(r['bytes'] for r in records):,} bytes")

if __name__ == "__main__":
    main()
