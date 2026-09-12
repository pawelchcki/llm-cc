# Comparison stages, version 1

All JSON uses UTF-8, sorted keys, compact separators, and finite numbers.
Python package: `tools.comparison` (stdlib except optional boto3 for S3).

Profile: `{scoring: {...}, build: {...}, max_file_bytes: 65536}`.
`scoring` contains `argv` (an ordered list of flags) and
`expected_configuration` (the scorer's requested/effective JSONL fields);
`build` includes source_commit, inference_abi, installed_files (relative path to
SHA256), model_sha256, model_bytes, and immutable execution image identity.
Container profiles also require `container_environment_policy: "sanitized-v1"`;
regenerate older profiles before preparing or reusing their cached results.
Scorer processes receive a clean environment with private cache/runtime paths.
Container workers preserve only scheduler device selectors (`CUDA_VISIBLE_DEVICES`,
`ROCR_VISIBLE_DEVICES`, `HIP_VISIBLE_DEVICES`, `GPU_DEVICE_ORDINAL`, and
`NVIDIA_VISIBLE_DEVICES`); loader overrides and inference tuning from `worker_env`
or the parent process do not reach the scorer. Container images must resolve their
runtime libraries through their normal loader configuration, without inherited
`LD_LIBRARY_PATH` overrides. Device allocation IDs do not affect cache identity.
Pools must supply the hardware supported by the scoring profile.
Bare ROCm profiles instead set `execution_image: "none"` and include
`execution_host: {gpu_vendor: "amd", gpu_arch, gpu_pci_address,
gpu_vram_bytes_min, resource_id, runtime_files: {absolute_path: sha256}}`.
Workers verify that host contract and hold the preprovisioned
`/var/lib/llm-cc/locks/<resource_id>.lock` during scoring. GPU labels are
scheduling hints; actual hardware validation and the lock enforce eligibility
and serialization. Bazzite uses one worker in the dedicated `linux-amd64-rocm`
pool. An optional absolute `execution_bundle` and `execution_bundle_sha256` in
the BuildBuddy configuration pin the comparison Python package on that host.
The package identity controls execution, while scorer cache keys remain based
on the complete scoring/build profile.
Fingerprint = SHA256(canonical JSON of `{scoring, build}`). Eligibility and
classification do not affect it. File key = SHA256(canonical JSON of
`[content_sha256, language, fingerprint]`).

Preparation `plan.json`:
```
{schema_version: 1,
 identity: {repository, pipeline_id, head_sha, target_sha, base_sha,
            target_branch, pr_number, started_at},
 fingerprint, profile,
 inventories: {base: [file], head: [file]},
 changes: [change],
 items: {key: {key, content_sha256, language, size, blob: "blobs/<sha256>"}},
 hits: {key: result}, workers: [{worker_id: 0, keys: [key], bytes: 123}],
 cache_stats: {...}}
```
Inventory file: `{path, language, category, size, content_sha256, key,
scorable, reason}`; unavailable values are null. Categories runtime, tests,
tooling; unsupported, excluded, oversized, symlink, submodule reasons retained.
Change: `{status, old_path, new_path}` with absent sides null.
Artifacts' blob paths are relative to plan directory. Plans are immutable.

Result: `{schema_version: 1, key, fingerprint, content_sha256, language,
llm_cc, token_count}`. Nonnegative finite llm_cc and nonnegative integer tokens.
Stored results are provenance checked against requested item and fingerprint.
Store interface `get(key)->bytes|None`, `put(key,bytes)`, `list(prefix)->[key]`.
ResultCache interface `get(item,fingerprint)->result|None`,
`put(result)`. Implement in cache.py; `open_store(location, **options)` and
`ResultCache(store, refresh_days=20, expire_days=30)`.

Worker entry `run_worker(plan_path, worker_id, store, output_dir,
scorer, model, installed_root, deadline_seconds=6600)` returns artifact dict:
`{schema_version:1, identity, fingerprint, worker_id, status:"complete"|"failed",
results:{key:result}, errors:[string], elapsed_seconds}`. Always write
`worker-<id>.json` in output_dir. Publish only after all invocations validate.

Preparation entry `prepare(repo, head, target, identity, profile, rules,
cache, output_dir, max_workers=4)` returns plan and writes plan.json/blobs.
Aggregation entry `aggregate(plan_path, worker_paths, output_dir)` validates
exact per-worker coverage, writes report.json/report.md/comment.md/publication.json
and returns report dict. Failure reports are mandatory, scores advisory.
CLI `python -m tools.comparison {prepare,worker,aggregate,compare}` exposes the
same stages through independently schedulable commands.

Cache object paths: `results/<file_key>.json` contains a checksum envelope over
the result, fingerprint, and refresh timestamp; `entropy/<fingerprint>/<name>.cbor`
contains an envelope with base64 payload, name, fingerprint, checksum, and time.
Workers restore only CBOR entries. BuildBuddy transport uses
`pipelines/<SHA256([repository,pipeline_id])>/` for the plan, blobs, per-worker
outputs and final reports. The submitted request pins the plan's SHA-256.

Publication envelope: `{schema_version:1, identity, fingerprint,
status:"complete"|"failed"|"incomplete", comment:{path:"comment.md",sha256,bytes}}`.
Identity has repository (`owner/repo`), pipeline_id (parent invocation UUID),
head_sha, target_sha, base_sha, target_branch, pr_number (null for baseline),
started_at (UTC ISO8601). Bound comment to 24 KiB UTF-8. Publisher validates
identity against its trusted event/publication set, owns marker and ordering.
