# Comparison stages, version 1

All JSON uses UTF-8, sorted keys, compact separators, and finite numbers.
Python package: `tools.comparison` (stdlib except optional boto3 for S3).

Profile: `{scoring: {...}, build: {...}, max_file_bytes: 65536,
inspection: {...}}`. Only `scoring` and `build` are fingerprinted;
`inspection.executable_identity_verified` records how the identity was
established, not what it is, so recording it never invalidates cached results.
`scoring` contains `argv` (an ordered list of flags) and
`expected_configuration` (the scorer's requested/effective JSONL fields);
`build` includes source_commit, inference_abi, installed_files (relative path to
SHA256), backend_manifest, model_sha256, model_bytes, optional model_url, and
immutable execution image identity. `source_commit` and `inference_abi` are
derived by hashing the installed tree and executing its `bin/llm-cc` offline
(`--version`, `cache status --format json`). The executable's version, its
reported `source_commit` and `backend_configuration`, the backend manifest and
the ABI's llama.cpp commit must all agree, and the executable's reported
`analysis_version` becomes the expected one. An executable predating those
reported fields records `inspection.executable_identity_verified: false` and the pinned
analysis version instead. `model_sha256`/`model_bytes` carry the scorer's
composite identity: for a split GGUF that is the domain-separated digest of
every shard and their total size, not the named shard alone. The executable
version is verified but deliberately not recorded, so the fingerprint of an
unchanged installation does not move.
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
Model digest memo: after verifying the model the worker writes
`<LLM_CC_ENTROPY_CACHE_DIR>/model-digests/<sha256 of the UTF-8 canonical model
path>.json` (directory `0700`, file `0600`) containing
`{format: "llm-cc-model-digest-memo-v1", size, mtime, ctime, device, inode,
digest}`, where the times are nanoseconds and the identifiers come from
`stat()`. llm-cc reuses `digest` only while all five stat fields still match,
and ignores keys it does not know; the contract is documented in
`src/model_identity.h`. The memo is advisory, is written only from a digest the
worker itself verified against the profile, sits outside `v2/entropy` so it is
never published, and dies with the worker's temporary directory. For a split
GGUF model the worker verifies and memoizes every shard, one memo per shard,
matching how the scorer memoizes them.
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
 rules, rules_source: {source: "host"} | {source: "repository", path, commit},
 presentation: {report_links: {name: url}},
 items: {key: {key, content_sha256, language, size, blob: "blobs/<sha256>"}},
 hits: {key: result}, workers: [{worker_id: 0, keys: [key], bytes: 123}],
 cache_stats: {...}}
```
Classification rules accept only `exclude`, `tests`, `tooling` (glob lists of
non-empty strings up to 256 characters), `extensions` (lowercase `.<ext>` to a
supported language) and `paths` (an ordered list of
`{pattern: <glob>, language: <supported>}` objects), 512 patterns in total
across the glob lists and `paths`, at most 64 KiB canonical. Language
resolution takes the first matching `paths` rule, then `extensions`, then the
built-in extension table; patterns match the full repository path with
`fnmatchcase`. The resolved language is part of the file key, so the same bytes
under two overridden directories are separate cache entries. Symlinks and
submodules keep their unscorable reason whatever rule matches them.
`prepare` reads `.llm-cc/comparison-rules.json` from the **target** commit's tree
when present, so a pull request cannot reclassify its own files; invalid
repository rules fail the run instead of falling back to host rules.
`presentation.report_links` holds already-substituted https URLs; the BuildBuddy
coordinator validates the templates and URL-quotes `{repository}`,
`{target_sha}` and `{target_branch}` before preparation.
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
cache, output_dir, max_workers=4,
repository_rules_path=".llm-cc/comparison-rules.json", presentation=None,
cache_concurrency=8)` returns plan and writes plan.json/blobs.
`cache_concurrency` (1-64) bounds parallel result-cache reads. The plan is
assembled from sorted keys, so it is byte-identical at any bound; the first
read error cancels queued reads and fails the run.
Aggregation entry `aggregate(plan_path, worker_paths, output_dir)` validates
exact per-worker coverage, writes report.json, report.md, comment.md,
publication.json, baseline.md and baseline.json, and returns report dict.

Report `report.json` stays `schema_version: 1`. Alongside the existing keys it
carries `rankings: {base: [entry], head: [entry]}` where an entry is
`{path, category, language, size, score, llm_cc, token_count, rank,
category_rank, changed}` over measured files with a positive token count,
ordered by descending score then path; `changed_files`, one row per change with a
score on either side, `{status, old_path, new_path, path, base: {category,
score} | null, head: {category, score, rank, category_rank} | null, delta,
percent}`; and the plan's `rules_source` and `presentation`. `leading_regressions`
and `leading_improvements` are derived from `changed_files`. Failure reports emit
empty `rankings` and `changed_files` and still write both baseline artifacts.

Baseline `baseline.json`: `{schema_version: 1, identity, fingerprint, status,
categories, rankings}` where `rankings` is the head side's ranking. `baseline.md`
opens with `Baseline ranking for <repository>@<head_sha> (<branch>)` and lists
head category scores, the top 50 files overall, the top 20 per category, and
unmeasured reasons. On a pull-request run these describe the PR head; the
default-branch copies retained by the publisher are the repository baseline.

Failure reports are mandatory, scores advisory.
CLI `python -m tools.comparison {prepare,worker,aggregate,compare}` exposes the
same stages through independently schedulable commands.

Cache object paths: `results/<file_key>.json` contains a checksum envelope over
the result, fingerprint, and refresh timestamp; `entropy/<fingerprint>/<name>.cbor`
contains an envelope with base64 payload, name, fingerprint, checksum, and time.
Workers restore only CBOR entries. BuildBuddy transport uses
`pipelines/<SHA256([repository,pipeline_id])>/` for the plan, blobs, per-worker
outputs and final reports (report.json, report.md, comment.md, publication.json,
baseline.md, baseline.json). The submitted request pins the plan's SHA-256.

Publication envelope: `{schema_version:1, identity, fingerprint,
status:"complete"|"failed"|"incomplete", comment:{path:"comment.md",sha256,bytes}}`.
Identity has repository (`owner/repo`), pipeline_id (parent invocation UUID),
head_sha, target_sha, base_sha, target_branch, pr_number (null for baseline),
started_at (UTC ISO8601). Bound comment to 24 KiB UTF-8. Publisher validates
identity against its trusted event/publication set, owns marker and ordering.
