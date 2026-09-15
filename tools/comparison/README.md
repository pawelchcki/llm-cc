# Revision comparison stages

The package compares committed source at two Git revisions. It is independent of
any CI provider and stores immutable stage artifacts in an explicit output
directory. All commands are also available as public Bazel launchers.

Create JSON files for the pipeline identity, scoring profile, and classification
rules, then prepare work:

```sh
bazel run //tools/comparison:prepare -- \
  --repo "$PWD" --head HEAD_SHA --target TARGET_SHA \
  --identity identity.json --profile profile.json --rules rules.json \
  --cache /shared/llm-cc-cache --output-dir artifacts
```

`plan.json` contains zero workers on a complete cache hit. Otherwise schedule one
GPU job per entry in `workers` and run its numeric `worker_id`:

```sh
bazel run //tools/comparison:worker -- \
  --plan artifacts/plan.json --worker-id 0 --cache /shared/llm-cc-cache \
  --output-dir artifacts --scorer /opt/llm-cc/bin/llm-cc --model /models/model.gguf \
  --installed-root /opt/llm-cc
```

The CPU aggregation stage accepts every produced worker artifact:

```sh
bazel run //tools/comparison:aggregate -- \
  --plan artifacts/plan.json --worker artifacts/worker-0.json \
  --output-dir artifacts/report
```

For local execution, `//tools/comparison:compare` accepts the union of the
prepare and worker flags and runs each planned worker sequentially. Cache
locations may be filesystem paths or `s3://bucket/prefix`. Pass S3 client
options as a JSON file with `--store-options`. `--refresh-days` and
`--expire-days` control result retention.

Every aggregation writes `report.json`, `report.md`, `comment.md`,
`publication.json`, `baseline.md` and `baseline.json`, including on validation
failure. An increased score is reported but does not make the command fail.
Missing or invalid analysis does.

## What the report contains

`comment.md` is the bounded pull-request comment, at most 24 KiB. It opens with
the status and one headline line per category, then the category table, the cache
line and any errors, a **Changed files** table of the paths this comparison
touched with their absolute base and head scores and the head's repository rank,
the leading regressions and improvements, and a collapsed **Top offenders on
the merge base** section listing the ten worst-scoring files at the merge base
with the rows this change touches marked. That section is the merge base, not
the target branch tip, so a branch behind its target reports the scores it is
actually compared against. Untrusted text is rendered inside balanced code
spans, so a path can never inject Markdown, a mention, or a link. When the
comment would exceed its limit, the lowest-priority sections are dropped first
and only then are whole lines cut; nothing is ever split inside a code span.

`report.md` carries the same sections without any truncation, plus the full
changed-files table, raw totals, coverage details and both inventories.
`report.json` adds `rankings` and `changed_files` to the existing keys; see
[SCHEMA.md](SCHEMA.md).

`baseline.md` and `baseline.json` describe the run's head revision on its own:
category scores, the 50 worst-scoring files overall, the 20 worst per category,
and the unmeasured paths. On a pull-request run they describe the PR head. The
copies published from default-branch pushes and retained by the publisher's
`keep_default_branch_sets` are the repository baseline. Set
`report_links.baseline` in the BuildBuddy configuration to that published URL and
pull-request comments will link to it.

## Per-repository classification rules

`prepare` reads `.llm-cc/comparison-rules.json` from the **target** commit's
tree. A pull request therefore cannot reclassify its own files, and rules that
fail validation fail the run instead of silently reverting to the host defaults.
Accepted keys are `exclude`, `tests`, `tooling` and `extensions`; glob lists hold
non-empty patterns of at most 256 characters, at most 512 patterns in total, and
the canonical document must stay under 64 KiB. Repositories without that file use
the rules the host publishes.

Run without Bazel using `python3 -m tools.comparison prepare` (Python 3.11+ and
Git). A consuming module can run `@llm_cc//tools/comparison:prepare`, including
when its `bazel_dep` uses a different `repo_name`. Each stage accepts explicit
paths; Bazel launchers resolve those paths in the original working directory.
For GitLab, upload `plan.json` and `blobs/` from preparation, generate zero to four
jobs from `plan.workers`, and run aggregation with `when: always`. Download worker
outputs into separate directories and pass every `worker-<id>.json` using repeated
`--worker` flags. Do not unpack preparation artifacts over completed worker files.

An identity file for a PR looks like:

```json
{
  "repository": "owner/repository",
  "pipeline_id": "parent-invocation-uuid",
  "target_branch": "release",
  "pr_number": 123,
  "started_at": "2026-09-11T12:00:00Z"
}
```

Preparation resolves the supplied head and actual target commits, calculates
their merge base, and adds all three SHAs to the identity. Fetch full history if
merge-base resolution fails. The package reads committed Git objects, including
headers; it neither scores working-tree changes nor follows symlinks/submodules.
Classification rules use `exclude`, `tests`, and `tooling` glob arrays plus an
optional extension-to-language map. Tests take precedence over tooling and
runtime. Every rule set is validated before use. See
[dogfood-rules.json](dogfood-rules.json) for the host rules and
[consumer/comparison-rules.json](consumer/comparison-rules.json) for a
per-repository example.

The dogfood scorer is pinned to
`4646123b274005c587dfeb614f17ddf5fd36aef6` for both revisions. Build and install
that revision in an independent checkout with explicit `source_commit` and
`source_version`, and the chosen CUDA or ROCm backend. Container workers use a
digest-pinned GPU image containing that installation and the checksum-pinned
DeepSeek model. Bazzite workers use the separately fingerprinted bare-host ROCm
profile described below.
This package does not provision storage or distribute/cache model weights.

Materialize the scoring identity from the final installed tree:

```sh
python3 -m tools.comparison.profile \
  --installed-root /opt/llm-cc \
  --execution-image 'registry.example/scorer@sha256:<64-hex-digest>' \
  --output profile.json
```

This `dogfood` preset checks the backend manifest's source commit and hashes
installed files. Any other scorer build uses the `generate` subcommand, which
derives the same identity instead of pinning it:

```sh
python3 -m tools.comparison.profile generate \
  --installed-root /opt/llm-cc --backend rocm \
  --execution-image 'registry.example/scorer@sha256:<64-hex-digest>' \
  --model /models/coder.gguf \
  --context 32768 --kv-cache-type q8_0 --output profile.json
```

`generate` accepts one flag per scoring setting, and takes the model identity
either from a local file (`--model`, hashed in place) or from explicit
`--model-sha256 --model-bytes [--model-url]`. Both subcommands run the installed
`bin/llm-cc` offline in a sanitized environment with private cache directories
(`--version` and `cache status --format json`) to read the executable's version
and inference ABI, and reject a tree whose executable, backend manifest and
llama.cpp commit disagree. Neither command downloads anything, but the generator
must run on a host that can execute the installed binary. `--flash-attn auto` and
`--entropy-reduction auto` are rejected because the scorer would then resolve
those settings against the runtime, leaving the expected configuration
non-deterministic. Optional `--expected-source-commit` and
`--expected-inference-abi` re-assert operator pins. `setup_bazzite.py` is
unaffected; it consumes an already generated profile file.
The CUDA profile pins the model's SHA-256 and 14,066,972,416-byte size, all
layers on GPU, context 131072, batch 256, Flash Attention on, Q8_0 K/V with
offload, device entropy reduction, structural hierarchy, tau 0.67 and alpha 0.8.
The default file limit is **65,536 bytes**; override it with `--max-file-bytes`.
Oversized supported files remain unmeasured, and affected category/repository
scores and totals are unavailable. Files are never truncated.

Fingerprint inputs are the complete scoring contract and installed/model/image
identity. Path, category, branch and application revision do not affect per-file
keys. Identical contents in one language share inference but each path counts
toward reporting. The repository/category score is `sum(llm_cc) / sum(tokens)`.
Zero-token scores and zero-baseline percentages are unavailable.

Filesystem writes use atomic replacement with owner/group permissions (`0660`)
restricted by the writer's umask/default ACL. For executor accounts sharing a
filesystem cache, provision its root with their shared group and mode `2770`,
and run each stage with umask `0007` so new
directories and objects remain accessible to that group. An owner-only deployment
can use umask `0077`. S3 requires the optional `boto3`
dependency in coordinator and worker environments. A `--store-options` JSON file
can contain `endpoint_url` and `region_name`; provide credentials through the AWS
environment/provider chain, never in plan artifacts. Read/authentication/transport
errors fail analysis; corrupt or mismatched cache entries become misses. Results
refresh after 20 days and expire after 30 days by default. Configure the bucket's
lifecycle accordingly. Native entropy entries have a separate namespace and are
published individually; locks, accounting, temporary files and model memos stay
private to each worker.

Workers verify the scorer, model, and source blobs before running one invocation
per language. Complete JSONL configuration, results, totals and process exit must
validate for every invocation before results are published. Each worker has a
110-minute deadline within a two-hour job, terminates scorer process groups on
cancellation, and retains separate JSONL/stderr logs and a structured artifact.
Linux workers also interrupt blocked store operations at the deadline and allow
30 seconds to upload their final artifact before logs. Platforms without POSIX
interval timers use cooperative deadline checks; live GPU workers require Linux.

The [BuildBuddy adapter](buildbuddy.py) uses the
[remote-run API](https://www.buildbuddy.io/docs/enterprise-api/#run) to submit only
planned misses. Configure [buildbuddy.example.json](buildbuddy.example.json) at
`LLM_CC_COMPARISON_CONFIG` on the CPU coordinator. `execution_commit` pins the
comparison implementation; `execution_image` must exactly match the scoring
profile. Bare-host profiles use `execution_image: "none"`, checksummed runtime
files and explicit GPU identity in `build.execution_host`. A checksummed
`execution_bundle` can pin a host-staged comparison package independently of
the repository under analysis. `report_links` holds https URL templates published
in pull-request comments; only `{repository}`, `{target_sha}` and
`{target_branch}` may appear, and their values are URL-quoted. Supply a registered GPU pool and shared store. Worker secrets use named
environment inputs sent as sensitive remote headers. The coordinator discovers
the current open PR and actual target, cancels obsolete workers, and retains
reports under a key derived from the **parent invocation** identity. Default-branch
pushes compare the head to itself and populate the baseline cache. A branch with
no current PR skips inference.

The `Complexity comparison` BuildBuddy action runs on pull requests targeting
`main` and on `main` pushes. This repository runs the checkout's own coordinator
through [dogfood.sh](dogfood.sh), so pull requests exercise coordinator changes
before the host bundle is refreshed; consuming repositories call the published
launcher instead. The coordinator derives its own head, branch and default branch
from the checkout, unwrapping BuildBuddy's synthetic merge commit to the actual
pull-request head. PR updates compare committed source against the merge base of
the actual target; `main` pushes populate the baseline cache and publish the
baseline ranking.
The installed Bazzite runner, store, and pinned scorer provide the execution
configuration. ci-toolkit consumes the completed BuildBuddy status and publishes
the generated table and report links using `.ci-toolkit.yml` from the PR's
target commit. The publication policy must land on the target branch before a
fresh PR comparison can publish automatically.

ci-toolkit owns comment markers, serialization, ordering, and PR-state rechecks;
these comparison stages never post comments. See
[ci-toolkit.example.yml](ci-toolkit.example.yml) for the publication policy.

## Adopting in another repository

Copy the two templates in [consumer/](consumer) into the repository, replace
`OWNER/REPO` in the BuildBuddy action, and merge the ci-toolkit automation on the
default branch first. See [consumer/README.md](consumer/README.md) for the
prerequisites, the shared-store trust note, and the `GITHUB_TOKEN` guidance.
Execution always happens on the Bazzite host through
`/var/lib/llm-cc/bin/llm-cc-coordinate`, which pins the verified package and its
immutable configuration generation.

Local acceptance is `bazel test //tools/comparison:comparison_test` and
`BACKEND=cpu tools/check_consumer.sh --tests-only`. Tests use synthetic scorers,
exercise cold/warm/incremental and one/four-worker comparisons, and assert zero
remote submissions on complete cache hits. A warm local comparison should take
under one minute; measure remote-store latency separately. Local correctness does
not establish GPU memory fit or live publication acceptance.

See [VALIDATION.md](VALIDATION.md) for executed checks and remaining live acceptance.

## Bazzite executor labels

The two host executors (`ccd0` and `ccd1`) expose the same Radeon. `ccd1` uses
the dedicated `linux-amd64-rocm` pool for dogfooding; `ccd0` retains the general
`linux-amd64-kvm` pool. Their shared host configuration advertises:

```yaml
gpu-vendor: amd
gpu-model: radeon-rx-7900-xtx
gpu-arch: gfx1100
gpu-vram-gib: "24"
gpu-runtime: rocm
gpu-sharing: shared
gpu-resource: bazzite-radeon-0
```

These labels describe the hardware; `gpu-runtime` identifies the required
backend, without asserting that a particular scorer image has been validated.
The matching `gpu-resource` value identifies one physical device across both
executors. The bare-host worker verifies the actual PCI/KFD device, architecture,
VRAM, access permissions and runtime hashes before scoring. It selects the
discrete GPU by ROCr UUID and holds a shared `flock` through all scoring
invocations. The scorer inherits the lock so abrupt worker death cannot release
the GPU while inference remains alive. Both executors use the same root-owned
lock inode under `/var/lib/llm-cc/locks`; setup never replaces it. Lock waiting
counts against the worker deadline. Other GPU applications must cooperate with
this lock to participate in serialization.

Continue selecting the executor pool explicitly. BuildBuddy's
[`debug-executor-labels` selector](https://github.com/buildbuddy-io/buildbuddy/blob/master/enterprise/server/scheduling/scheduler_server/scheduler_server.go)
uses best-effort routing and can fall back when labels do not match; it is not
a strict GPU eligibility or concurrency constraint.

The Bazzite profile uses ROCm, context **32768**, batch **256**, Flash Attention
on and **Q8_0** K/V; all other scoring settings and the **64 KiB** file limit
match the pinned dogfood contract. Context, backend, host runtime and GPU
identity affect the fingerprint. The host configuration uses one worker because
the two builders share one Radeon. The CPU coordinator uses `linux-amd64-rocm`
and the existing host filesystem cache. Follow [Bazzite setup](BAZZITE.md) to
install the verified assets and publish an atomic configuration generation.
