#!/usr/bin/env python3
"""Run real llm-cc inference sequentially; reject partial or cached timing runs."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import threading
import time

def gpu_memory(stop, samples):
    while not stop.is_set():
        result = subprocess.run(
            ["nvidia-smi", "--id=0", "--query-gpu=memory.used", "--format=csv,noheader,nounits"],
            capture_output=True, text=True)
        for line in result.stdout.splitlines():
            if line.strip().isdigit():
                samples.append(int(line.strip()))
        stop.wait(0.5)

def run(root, binary, model, label, extra, expect_hits):
    prefix = root / "results" / (model["name"] + "--" + label)
    corpus = root / "corpus"
    manifest = json.loads((root / "corpus.json").read_text())
    expected = {row["id"] for row in manifest["files"]}
    paths = [str(corpus / row["id"]) for row in manifest["files"]]
    command = [str(binary), *paths, "--model", str(root / "models" / model["file"]),
               "--no-download", "--gpu-layers", "-1", "--backend", "cuda",
               "--context", "32768", "--batch-size", "256", "--entropy-reduction", "device",
               "--hotspots", "10", "--format", "jsonl", "--progress", "always", *extra]
    previous = Path(str(prefix) + ".json")
    if previous.exists():
        result = json.loads(previous.read_text())
        if not result["valid"] or result["command"] != command:
            raise RuntimeError(f"Previous run is invalid or has different settings: {previous}")
        print(f"Already complete: {prefix.name}", flush=True)
        return
    environment = os.environ | {"LLM_CC_ENTROPY_CACHE_DIR": str(root / "runtime-cache"),
                               "CUDA_VISIBLE_DEVICES": "0"}
    stop, samples = threading.Event(), []
    idle_gpu_mib = int(subprocess.check_output(
        ["nvidia-smi", "--id=0", "--query-gpu=memory.used", "--format=csv,noheader,nounits"], text=True).strip())
    monitor = threading.Thread(target=gpu_memory, args=(stop, samples), daemon=True)
    monitor.start()
    events, files, timings = [], [], {}
    started = time.monotonic()
    with Path(str(prefix) + ".stderr").open("w") as error:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=error,
                                   text=True, env=environment)
        for line in process.stdout:
            event = json.loads(line)
            now = time.monotonic()
            events.append(event)
            if event["type"] == "file_start":
                timings[event["path"]] = now
            if event["type"] == "file":
                row = {key: value for key, value in event.items() if key != "units"}
                row["id"] = str(Path(event["path"]).relative_to(corpus))
                row["wall_seconds"] = now - timings[event["path"]]
                files.append(row)
                print(f"{model['name']} {label}: {row['id']} {row['wall_seconds']:.2f}s", flush=True)
        status = process.wait()
    elapsed = time.monotonic() - started
    stop.set()
    monitor.join()
    with gzip.open(Path(str(prefix) + ".jsonl.gz"), "wt") as output:
        for event in events:
            output.write(json.dumps(event, separators=(",", ":")) + "\n")
    totals = [event for event in events if event["type"] == "totals"]
    valid = (status == 0 and len(totals) == 1 and totals[0]["partial"] is False
             and events[-1]["type"] == "totals" and len(files) == len(expected)
             and {row["id"] for row in files} == expected
             and all(row["entropy_cache_hit"] == expect_hits for row in files))
    record = dict(model=model["name"], label=label, command=command, exit_code=status,
                  valid=valid, wall_seconds=elapsed, files=files, totals=totals,
                  configuration=[event for event in events if event["type"] == "configuration"],
                  idle_gpu_mib=idle_gpu_mib,
                  peak_gpu_mib=max(samples, default=idle_gpu_mib) - idle_gpu_mib,
                  gpu_memory_method="sampled device-0 memory.used minus pre-run idle; 0.5-second interval")
    Path(str(prefix) + ".json").write_text(json.dumps(record, indent=2) + "\n")
    if not valid:
        raise RuntimeError(f"Incomplete run or unexpected cache state: {prefix}")
    print(f"COMPLETE {model['name']} {label}: {elapsed:.2f}s", flush=True)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--model", action="append")
    parser.add_argument("--repetitions", type=int, default=2)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent)
    args = parser.parse_args()
    if args.repetitions < 1:
        parser.error("--repetitions must be positive")
    root = args.root.resolve()
    (root / "results").mkdir(exist_ok=True)
    binary = args.binary.resolve()
    metadata = dict(platform=platform.platform(), binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                    binary_version=subprocess.check_output([str(binary), "--version"], text=True).strip(),
                    gpu=subprocess.check_output(["nvidia-smi", "--query-gpu=name,driver_version,memory.total",
                                                 "--format=csv,noheader"], text=True).strip())
    environment_file = root / "environment.json"
    if environment_file.exists() and json.loads(environment_file.read_text()) != metadata:
        raise RuntimeError("Binary or environment changed; use a fresh results directory")
    environment_file.write_text(json.dumps(metadata, indent=2) + "\n")
    for row in json.loads((root / "corpus.json").read_text())["files"]:
        if hashlib.sha256((root / "corpus" / row["id"]).read_bytes()).hexdigest() != row["sha256"]:
            raise RuntimeError(f"Corpus checksum mismatch: {row['id']}")
    models = json.loads((root / "models.json").read_text())
    unknown = set(args.model or []) - {model["name"] for model in models}
    if unknown:
        parser.error(f"Models are absent from models.json: {', '.join(sorted(unknown))}")
    for model in models:
        if args.model and model["name"] not in args.model:
            continue
        # First pass fills an isolated entropy cache; timing must be entirely misses.
        run(root, binary, model, "default-r1", ["--tau", "0.67"], False)
        for repeat in range(2, args.repetitions + 1):
            run(root, binary, model, f"default-r{repeat}", ["--tau", "0.67", "--no-cache"], False)
        for label, options in [("tau-0.4", ["--tau", "0.4"]), ("tau-1.0", ["--tau", "1.0"]),
                               ("percentile-80", ["--tau-percentile", "80"]),
                               ("structural-only", ["--tau", "100"]),
                               ("reference", ["--hierarchy", "reference"])]:
            run(root, binary, model, label, options, True)

if __name__ == "__main__":
    main()
