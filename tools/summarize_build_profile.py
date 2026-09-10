"""Summarize a Bazel JSON trace and build-event JSON benchmark pair."""
import argparse
import gzip
import json
import re
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("profile", type=Path)
parser.add_argument("events", type=Path)
args = parser.parse_args()
with gzip.open(args.profile, "rt") as stream:
    profile = json.load(stream)
actions = sorted(
    [e for e in profile["traceEvents"] if e.get("cat") == "action processing" and ".cu" in e.get("name", "")],
    key=lambda e: e.get("dur", 0), reverse=True,
)
metrics = {}
for line in args.events.read_text().splitlines():
    event = json.loads(line)
    if "buildMetrics" in event:
        metrics = event["buildMetrics"]
log = args.events.with_name(args.events.name.replace(".events.jsonl", ".log")).read_text()
match = re.search(r"Elapsed time: ([0-9.]+)s, Critical Path: ([0-9.]+)s", log)
print(json.dumps({
    "elapsed_seconds": float(match[1]) if match else None,
    "critical_path_seconds": float(match[2]) if match else None,
    "timing": metrics.get("timingMetrics", {}),
    "actions": metrics.get("actionSummary", {}),
    "expensive_cuda_actions": [{"name": e["name"], "seconds": e["dur"] / 1e6} for e in actions[:10]],
}, indent=2))
