#!/usr/bin/env python3
"""Run isolated, uncached inference configurations and retain full evidence."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import threading
import time

CONFIGS = {
    "baseline": ("auto", "f16"),
    "flash-f16": ("on", "f16"),
    "flash-q8": ("on", "q8_0"),
    "flash-q4": ("on", "q4_0"),
}


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def gpu_sample(backend, environment):
    if backend == "cuda":
        result = subprocess.run(
            ["nvidia-smi", "--id=0", "--query-gpu=memory.used,utilization.gpu",
             "--format=csv,noheader,nounits"], capture_output=True, text=True,
            env=environment)
        match = re.search(r"(\d+)\s*,\s*(\d+)", result.stdout)
        return tuple(map(int, match.groups())) if match else None
    result = subprocess.run(
        ["rocm-smi", "--device", "0", "--showmeminfo", "vram",
         "--showuse", "--json"],
        capture_output=True,
        text=True,
        env=environment,
    )
    if result.returncode != 0:
        return None
    try:
        devices = json.loads(result.stdout).values()
        device = next(iter(devices), None)
        if device is None:
            return None
        used_mib = int(device["VRAM Total Used Memory (B)"]) // (1024 * 1024)
        utilization = int(device["GPU use (%)"])
    except (AttributeError, KeyError, TypeError, ValueError):
        return None
    return used_mib, utilization


def monitor(stop, pid, backend, samples, started, environment):
    while not stop.is_set():
        rss_kib = None
        try:
            for line in Path(f"/proc/{pid}/status").read_text().splitlines():
                if line.startswith("VmRSS:"):
                    rss_kib = int(line.split()[1])
        except (FileNotFoundError, ProcessLookupError):
            pass
        try:
            gpu = gpu_sample(backend, environment)
        except FileNotFoundError:
            gpu = None
        samples.append({"elapsed": time.monotonic() - started, "rss_kib": rss_kib,
                        "gpu_memory": gpu[0] if gpu else None,
                        "gpu_utilization": gpu[1] if gpu else None})
        stop.wait(0.5)


def failure_class(status, stderr, errors):
    lower = "\n".join(
        [stderr, *(str(event.get("message", "")) for event in errors)]
    ).lower()
    if "flash_attn_ext@cpu" in lower or "would run on cpu" in lower:
        return "cpu_attention_fallback"
    if status == 0:
        return None
    if "out of memory" in lower or "allocation" in lower and "failed" in lower:
        return "oom"
    if "exceeds --context" in lower or "token count" in lower and "exceeds" in lower:
        return "token_limit"
    return "failed"


def configuration_matches(event, args, flash, kv_type):
    expected = {
        "context": args.context,
        "batch_size": args.batch_size,
        "entropy_reduction": "device",
        "effective_entropy_reducer": "device",
        "flash_attn": flash,
        "effective_flash_attn": flash,
        "kv_cache_type": kv_type,
        "effective_kv_cache_type": kv_type,
        "kv_offload": "on",
        "effective_kv_offload": "on",
        "backend_diagnostics": True,
        "gpu_layers": -1,
        "no_download": True,
        "progress": "always",
        "hotspots": 10,
    }
    cache = event.get("cache")
    return (all(event.get(key) == value for key, value in expected.items())
            and isinstance(cache, dict) and cache.get("enabled") is False)


def backend_hashes(directory, backend):
    hashes = {
        path.name: sha256_file(path)
        for path in sorted(directory.iterdir())
        if path.is_file()
        and backend in path.name
        and path.suffix in (".bundle", ".so", ".dylib", ".dll")
    }
    if not hashes:
        raise RuntimeError(f"no {backend} backend artifact in {directory}")
    return hashes


def select_rows(manifest, scope):
    if scope == "historical":
        return [row for row in manifest if row["group"] != "fixture"]
    if scope == "fixtures":
        return [row for row in manifest if row["group"] == "fixture"]
    if scope == "fixture-206":
        return [row for row in manifest if row["id"].endswith("-206k.c")]
    return manifest


def execute(args, root, model, label, repeat, rows, invocation,
            invocation_fingerprint):
    flash, kv_type = CONFIGS[label]
    stem = f"{model['name']}--{args.backend}--{args.scope}--{label}-r{repeat}"
    prefix = root / "results" / stem
    runtime_cache = root / "runtime-cache" / stem
    runtime_cache.mkdir(parents=True, exist_ok=True)
    paths = [str(root / "corpus" / row["id"]) for row in rows]
    command = [str(args.binary), *paths, "--model", str(root / "models" / model["file"]),
               "--no-download", "--no-cache", "--gpu-layers", "-1",
               "--backend", args.backend, "--context", str(args.context),
               "--batch-size", str(args.batch_size), "--entropy-reduction", "device",
               "--flash-attn", flash, "--kv-cache-type", kv_type,
               "--kv-offload", "on", "--backend-diagnostics", "--hotspots", "10",
               "--format", "jsonl", "--progress", "always"]
    if args.backend_dir:
        command += ["--backend-dir", str(args.backend_dir)]
    environment = os.environ | {"LLM_CC_ENTROPY_CACHE_DIR": str(runtime_cache)}
    visibility = {"LLM_CC_ENTROPY_CACHE_DIR": str(runtime_cache)}
    if args.backend == "cuda":
        environment["CUDA_VISIBLE_DEVICES"] = "0"
        visibility["CUDA_VISIBLE_DEVICES"] = "0"
    elif args.backend == "rocm":
        environment["ROCR_VISIBLE_DEVICES"] = "0"
        visibility["ROCR_VISIBLE_DEVICES"] = "0"
    started = time.monotonic()
    stderr_path = Path(str(prefix) + ".stderr")
    output_path = Path(str(prefix) + ".jsonl.gz")
    files, configurations, error_events, totals, parse_errors = [], [], [], [], []
    events_count, last_type = 0, None
    with stderr_path.open("w") as error_stream, gzip.open(output_path, "wt") as output:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=error_stream,
                                   text=True, env=environment)
        stop, samples = threading.Event(), []
        watcher = threading.Thread(target=monitor,
                                   args=(stop, process.pid, args.backend, samples, started,
                                         environment), daemon=True)
        watcher.start()
        try:
            for line in process.stdout:
                output.write(line)
                events_count += 1
                try:
                    event = json.loads(line)
                    if not isinstance(event, dict):
                        raise ValueError("event is not a JSON object")
                except json.JSONDecodeError as error:
                    parse_errors.append({"line": events_count, "message": str(error)})
                    continue
                except ValueError as error:
                    parse_errors.append({"line": events_count, "message": str(error)})
                    continue
                last_type = event.get("type")
                if event.get("type") == "configuration":
                    configurations.append(event)
                elif event.get("type") == "file":
                    files.append({key: value for key, value in event.items() if key != "units"})
                elif event.get("type") == "error":
                    error_events.append(event)
                elif event.get("type") == "totals":
                    totals.append(event)
            status = process.wait()
        finally:
            stop.set()
            watcher.join()
    elapsed = time.monotonic() - started
    stderr = stderr_path.read_text(errors="replace")
    expected = {str(root / "corpus" / row["id"]) for row in rows}
    configuration_valid = (len(configurations) == 1 and
                           configuration_matches(configurations[0], args, flash, kv_type))
    valid = (status == 0 and not parse_errors and last_type == "totals" and len(totals) == 1
             and configuration_valid
             and totals[0].get("partial") is False
             and len(files) == len(rows) and {row.get("path") for row in files} == expected
             and all(row.get("entropy_cache_hit") is False for row in files)
             and "FLASH_ATTN_EXT@CPU" not in stderr)
    classification = failure_class(status, stderr, error_events)
    if parse_errors:
        classification = "incomplete_output"
    elif status == 0 and classification is None and not configuration_valid:
        classification = "configuration_mismatch"
    elif status == 0 and classification is None and not valid:
        classification = "incomplete_output"
    host_rss = [sample["rss_kib"] for sample in samples
                if sample["rss_kib"] is not None]
    gpu_memory = [sample["gpu_memory"] for sample in samples
                  if sample["gpu_memory"] is not None]
    gpu_utilization = [sample["gpu_utilization"] for sample in samples
                       if sample["gpu_utilization"] is not None]
    record = {
        "model": model["name"], "backend": args.backend, "scope": args.scope,
        "label": label, "repeat": repeat, "command": command,
        "environment_overrides": visibility,
        "exit_code": status, "failure_class": classification,
        "valid": valid, "wall_seconds": elapsed, "event_count": events_count,
        "files": files, "errors": error_events, "parse_errors": parse_errors,
        "totals": totals,
        "configuration": configurations,
        "invocation": invocation,
        "invocation_fingerprint": invocation_fingerprint,
        "model_sha256": model["sha256"],
        "corpus_sha256": sha256_file(root / "corpus.json"),
        "peak_host_rss_kib": max(host_rss, default=None),
        "peak_gpu_memory": max(gpu_memory, default=None),
        "mean_gpu_utilization": (sum(gpu_utilization) / len(gpu_utilization)
                                  if gpu_utilization else None),
        "gpu_sample_units": "MiB for CUDA and ROCm",
        "samples": samples,
        "phase_lines": [line for line in stderr.splitlines()
                        if line.startswith("llm-cc: ")],
    }
    Path(str(prefix) + ".json").write_text(json.dumps(record, indent=2) + "\n")
    print(f"{stem}: {'complete' if valid else record['failure_class']} ({elapsed:.1f}s)", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--backend", choices=("cuda", "rocm"), required=True)
    parser.add_argument("--backend-dir", type=Path, required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--context", type=int, default=32768)
    parser.add_argument("--batch-size", type=int, default=256)
    parser.add_argument("--scope", choices=("historical", "fixtures", "fixture-206", "all"),
                        default="historical")
    parser.add_argument("--configuration", action="append", choices=tuple(CONFIGS))
    args = parser.parse_args()
    if args.repetitions < 1 or args.context < 1 or args.batch_size < 1:
        parser.error("repetitions, context, and batch size must be positive")
    args.root = args.root.resolve()
    args.binary = args.binary.resolve()
    args.backend_dir = args.backend_dir.resolve() if args.backend_dir else None
    (args.root / "results").mkdir(exist_ok=True)
    manifest = json.loads((args.root / "corpus.json").read_text())["files"]
    rows = select_rows(manifest, args.scope)
    if not rows:
        parser.error(
            f"--scope {args.scope} selects no files from corpus.json; "
            "choose --scope fixtures or --scope all for a fixtures-only corpus"
        )
    for row in rows:
        path = args.root / "corpus" / row["id"]
        if path.stat().st_size != row["bytes"] or sha256_file(path) != row["sha256"]:
            raise RuntimeError(f"corpus checksum mismatch: {row['id']}")
    models = json.loads((args.root / "models.json").read_text())
    model = next((row for row in models if row["name"] == args.model), None)
    if model is None:
        parser.error("model is absent from models.json")
    model_path = args.root / "models" / model["file"]
    if model_path.stat().st_size != model["bytes"] or sha256_file(model_path) != model["sha256"]:
        raise RuntimeError("model checksum mismatch")
    labels = args.configuration or list(CONFIGS)
    corpus_sha256 = sha256_file(args.root / "corpus.json")
    environment = {"platform": platform.platform(),
                   "binary_sha256": sha256_file(args.binary),
                   "binary_version": subprocess.check_output(
                       [str(args.binary), "--version"], text=True).strip(),
                   "backend": args.backend,
                   "backend_sha256": backend_hashes(args.backend_dir, args.backend)}
    invocation = {
        **environment,
        "model": model["name"],
        "model_sha256": model["sha256"],
        "corpus_sha256": corpus_sha256,
        "scope": args.scope,
        "files": [{"id": row["id"], "sha256": row["sha256"]} for row in rows],
        "context": args.context,
        "batch_size": args.batch_size,
        "repetitions": args.repetitions,
        "configurations": labels,
    }
    invocation_fingerprint = hashlib.sha256(
        json.dumps(invocation, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    environment["invocation"] = invocation
    environment["invocation_fingerprint"] = invocation_fingerprint
    (args.root / "results" / f"environment-{args.backend}.json").write_text(
        json.dumps(environment, indent=2) + "\n")
    for label in labels:
        for repeat in range(1, args.repetitions + 1):
            execute(args, args.root, model, label, repeat, rows, invocation,
                    invocation_fingerprint)


if __name__ == "__main__":
    main()
