# Comparison stages, version 2

`llm-cc compare` implements selection, classification, planning, scoring,
caching and reporting; `src/compare/` is the reference. The Python package
`tools.comparison` only discovers pull requests, generates CI jobs, stores and
publishes reports, and drives BuildBuddy. All JSON uses UTF-8, sorted keys,
compact separators and finite numbers ("canonical": Python's
`json.dumps(sort_keys=True, separators=(",", ":"), ensure_ascii=True)`).
Version 2 shares nothing with version 1: its plans, results and cache objects
live under new names, and version 1 artifacts are refused.

## Identity and keys

A plan names three objects, and every stage must agree on them:

- `scorer`: the running build, `{version, commit, executable,
  backend_configuration, inference_abi, analysis_version}`. `commit` is the
  stamped source commit; an unstamped build has `commit: null` and
  `executable`, the SHA-256 of its own executable, instead. A worker refuses a
  plan whose `scorer` differs from its own, so the coordinator and the GPU
  workers must run the same llm-cc build.
- `model`: `{sha256, bytes}`, the scorer's model identity. A split GGUF has
  the domain-separated digest of all its shards and their total size.
- `scoring`: every setting that changes a result, `{backend, context,
  batch_size, entropy_reduction, flash_attn, kv_cache_type, kv_offload,
  hierarchy, tau, tau_percentile, alpha}`. `backend` is the cache identity
  (`cpu` or `<backend>/gpu-layers=<n>`); `tau` is the explicit threshold, else
  the calibration of a registered model with this digest, else 0.67, and null
  under `tau_percentile`. `auto` backend, entropy reduction or flash attention
  are refused outside `compare run`, which resolves them on its own machine.

Fingerprint = `sha256("llm-cc-compare-fingerprint-v2\0" + canonical({scorer,
model, scoring}))`. Result key = `sha256("llm-cc-compare-result-v2\0" +
blob_id + "\0" + language + "\0" + fingerprint)`, where `blob_id` is the Git
object id of the file's bytes (SHA-1, or SHA-256 in SHA-256 repositories).
Classification, eligibility and presentation do not affect either.

`llm-cc compare identity MODEL SCORING` prints `{scorer, model, scoring,
fingerprint}`.

## Plan

`prepare` writes `blobs/<blob_id>` for every miss, then `plan.json`:

```
{schema_version: 2,
 identity: {repository, pipeline_id, head_sha, target_sha, base_sha,
            target_branch, pr_number, started_at},
 scorer, model, scoring, fingerprint,
 max_file_bytes,
 rules, rules_source: {source: "repository", path, commit}
                    | {source: "host"} | {source: "builtin"},
 presentation: {report_links: {name: url}},
 inventories: {base: [file], head: [file]},
 changes: [{status, old_path, new_path}],
 items: {key: {key, blob_id, language, size}},
 hits: {key: result},
 workers: [{worker_id, keys: [key], bytes}],
 cache_stats: {items, hits, misses, hit_bytes, miss_bytes}}
```

- `identity` comes from `--identity`: `repository` and `pipeline_id` are
  required, `target_branch`, `pr_number` and `started_at` optional. The commit
  fields are resolved from `--head`, `--target` and their merge base; an
  identity file may carry them only as null or as those same commits.
- An inventory file is `{path, language, category, size, blob_id, key,
  scorable, reason}` for every tree entry, with unavailable values null.
  `category` is runtime, tests or tooling; `reason` explains an unscorable
  path: submodule, symlink, unsupported, excluded or oversized (larger than
  `max_file_bytes`). A path that is not UTF-8 is recorded with `\xHH` escapes
  and is unsupported.
- `items` holds each distinct key once; the same bytes in one language are one
  item wherever they appear.
- `workers` partitions the misses largest file first, each to the worker with
  the fewest bytes (then the lowest id), over at most four workers. It is empty
  when every item is cached; CI generates no GPU job then.
- `rules` are the effective rules and `rules_source` where they came from: the
  **target** commit's `.llm-cc/rules.json` (or the legacy
  `.llm-cc/comparison-rules.json`), so a pull request cannot reclassify its own
  files; else `--default-rules`; else the built-ins. Invalid repository rules
  fail the run. The rules syntax is `llm-cc rules`' (see the README).
- `presentation.report_links` holds already substituted https URLs.

Plans are validated whole by every later stage: the fingerprint must match the
three identity objects, every item key its provenance, every scorable
inventory file an item, and the workers must cover exactly the misses.

## Results and workers

A result is `{key, blob_id, language, fingerprint, llm_cc, token_count,
high_entropy_tokens, entropy_sum, total_branch, total_comp_level}`, the inputs
llm-cc's totals need; numbers are finite and non-negative and
`high_entropy_tokens <= token_count`. A result counts only for the item and
fingerprint it names.

`worker` scores its assignment with one model session. It checks each blob
against its id, stores each result as soon as it exists, and always writes
`worker-<id>.json`:

```
{schema_version: 2, identity, fingerprint, worker_id,
 status: "complete" | "failed", results: {key: result}, errors: [string],
 elapsed_seconds}
```

`complete` means every assigned key has a result and nothing failed. A file
that cannot be scored is an error for that file; a wrong model, build, backend,
store or deadline (`--deadline-seconds`, default 6600) ends the worker.

With `--execution-host FILE`, a Linux worker first verifies a bare AMD host:
`{gpu_vendor: "amd", gpu_pci_address, gpu_arch, gpu_vram_bytes_min,
resource_id, runtime_files: {absolute_path: sha256}}`. The runtime files, the
PCI vendor, exactly one KFD node at that address with that architecture and
VRAM, its ROCm UUID and read-write `/dev/kfd` and render node must all match.
It then holds the preprovisioned, root-owned
`/var/lib/llm-cc/locks/<resource_id>.lock` while scoring and restricts ROCm to
that GPU with `ROCR_VISIBLE_DEVICES=GPU-<uuid>`.

## Store

A store is a directory or `s3://bucket/prefix`, read and written whole with
GET and PUT only. A missing object is a miss; any other failure fails the
stage. Filesystem objects are replaced atomically with mode 0660 before the
umask. S3 requests are signed with SigV4; credentials come only from
`AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY` and `AWS_SESSION_TOKEN`, and
`--store-options` may carry only `{endpoint_url, region_name}` (else
`AWS_ENDPOINT_URL_S3` or `AWS_ENDPOINT_URL`, and `AWS_REGION`,
`AWS_DEFAULT_REGION` or us-east-1).

- `results/v2/<key>.json`: `{schema_version: 2, stored_at, sha256, result}`
  where `sha256` covers the canonical result. An entry that fails any check is
  a miss. A hit older than 20 days (`--refresh-days`) is rewritten with a new
  `stored_at`; one older than 30 (`--expire-days`) is a miss.
- `entropy/v2/<entry key>.cbor`: llm-cc's native entropy cache entry for one
  file under one model and inference configuration. Workers read it before
  inference and write it after, so a new threshold or presentation rescores
  from stored entropy instead of the model. A corrupt entry is rescored.
- `pipelines/<sha256(canonical([repository, pipeline_id]))>/`: a pipeline's
  reports, written by `store-report` under the identity inside the report, so
  a child pipeline stores under its parent's ID. BuildBuddy transport also
  keeps the plan, blobs and worker outputs there; the worker request pins the
  plan's SHA-256.
- `publications/<sha256(canonical([repository, pr_number]))>.json`: the
  publication marker `{schema_version: 1, ordinal, pipeline_id, head_sha,
  state: "reserved"|"published"|"suppressed", comment_id: int|null}`, written
  only by the Python publisher with conditional writes (`put_if`: ETag
  `If-Match`/`If-None-Match: *` on S3, a per-key `flock` on a filesystem).

Writes are idempotent; concurrent writers of one key store equal content, and
the last one wins.

## Reports

`aggregate` validates the plan and every worker artifact (identity,
fingerprint, schema, exact key coverage, status and each result) and writes
`report.json`, `report.md`, `comment.md`, `baseline.md`, `baseline.json` and
`publication.json`. A worker that fails any check contributes nothing and
becomes an error. Each `--error` turns the report into a failure while keeping
any aggregated plan in `analysis-report.json`; without a plan, `--identity`
names the pipeline.

`report.json` is `schema_version: 2` with `identity`, `fingerprint`, `scorer`,
`model`, `scoring`, `status` ("complete", "incomplete" or "failed"),
`inventories`, `results`, `sides`, `comparisons` (runtime, tests, tooling,
repository), `changes`, `change_counts`, `rankings: {base, head}`,
`changed_files`, `leading_regressions`, `leading_improvements`,
`rules_source`, `presentation`, `cache_stats` and `errors`.

- A side totals each category like `llm-cc` totals a run; a category or side
  has an LM-CC total only when every supported path was measured, so an
  oversized runtime file leaves the runtime total unavailable and the report
  incomplete.
- A ranking entry is `{path, category, language, size, score, llm_cc,
  token_count, rank, category_rank, changed}` over measured files, by
  descending `llm_cc`, then path.
- `changed_files` has one row per change measured on either side: `{status,
  old_path, new_path, path, base: {category, score, llm_cc} | null, head:
  {category, score, llm_cc, rank, category_rank} | null, delta, percent,
  raw_delta, raw_percent}`.
- `score`, `delta` and `percent` are per token and null without tokens; tables,
  rankings and the leading changes use total `llm_cc` and `raw_delta` /
  `raw_percent`.

`comment.md` is at most 24 KiB of UTF-8: sections are dropped from the least
important first. Repository-controlled text is neutralized: control and
bidirectional characters, mentions and Markdown syntax cannot escape their
code spans. The comment ends by naming its rules: `Rules: repository
<path>@<commit>`, `Rules: host` or `Rules: built-in`.

`baseline.json` is `{schema_version: 2, identity, fingerprint, status,
categories, rankings}` with the head side's ranking; `baseline.md` opens with
`Baseline ranking for <repository>@<head_sha> (<branch>)` and lists the head
category scores, the top 50 files, the top 20 per category and the unmeasured
reasons. On a default-branch run these are the repository baseline.

`publication.json` is `{schema_version: 2, identity, fingerprint, status,
comment: {path: "comment.md", sha256, bytes}}`. Failure reports are mandatory,
scores advisory.

## Provider glue

Discovery writes the identity above with `base_sha: null`, or `skipped.json`
`{reason, identity}` for a branch without a current pull request. `--dotenv`
appends `COMPARISON_SKIP=0|1` and, unless skipped, `COMPARISON_PR_NUMBER`
(omitted for the default branch) and `COMPARISON_TARGET_SHA`.

Recipe configuration for `ci` is optional: `{schema_version: 1, images:
{coordinator?, scorer?}, scorer: {executable, model}, llm_cc, max_workers,
cache_concurrency, worker_timeout, aggregate_timeout, artifact_expiry, gitlab:
{prepare_job, cpu_tags, gpu_tags}}`. Images are `name@sha256:<64 hex>`;
`images.scorer` defaults to the `LLM_CC_SCORER_IMAGE` the coordinator image was
built with. `ci gitlab-child` rejects a plan with more workers than
`max_workers`.

The publisher trusts `--repository`, `--pipeline-id` and `--head` from CI and
posts `<!-- llm-cc-comparison -->`, then `<!-- llm-cc-comparison
pipeline=<id> ordinal=<n> -->`, then `comment.md`. It updates only a comment by
`--comment-author` whose first line is that marker, and never one whose
recorded ordinal exceeds its own.
