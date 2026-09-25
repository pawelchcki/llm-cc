# Revision comparison

`llm-cc compare` compares committed source at two Git revisions: it selects and
classifies files, plans the work, scores it, caches results and renders the
report. This package is the provider glue around it: GitHub discovery and
publication, CI pipeline generation, and the BuildBuddy/Bazzite coordinator.

For a local comparison, one command runs every stage in process:

```sh
llm-cc compare run --target main --output-dir comparison \
  --model /models/model.gguf
```

`--head` defaults to `HEAD`, `--backend auto` and `--entropy-reduction auto`
are resolved on this machine, and results are cached under `compare/` beside
the model cache unless `--cache DIR|s3://bucket/prefix` names another store.
A rerun scores nothing that is already cached.

## CI stages

CI splits the same comparison across a CPU coordinator and zero to four GPU
workers, which must all run the same llm-cc build. Preparation takes the
scoring settings explicitly, usually from a response file with one argument
per line:

```sh
llm-cc compare prepare --repo "$PWD" --head HEAD_SHA --target TARGET_SHA \
  --identity identity.json --cache s3://bucket/llm-cc --output-dir prep \
  @scoring.args
```

`scoring.args` pins the model and every scoring setting, for example:

```text
--model-sha256
5a2e25280075d769abdb111de8211d9d3367f2ae0d0e6166a288ee6e8ed0345d
--model-bytes
14066972416
--backend
cuda
--entropy-reduction
device
--flash-attn
on
```

`--model GGUF` or `--model-name NAME` hashes a local model instead; `auto`
settings are refused, because the coordinator is not the GPU host. Planning
options are `--max-workers` (at most 4), `--max-file-bytes` (default 65536),
`--default-rules FILE`, `--presentation FILE`, `--cache-concurrency`,
`--refresh-days` and `--expire-days`. `llm-cc compare identity @scoring.args`
prints the scorer, model, scoring and fingerprint a plan would carry.

`prep/plan.json` lists zero workers on a complete cache hit. Otherwise run one
GPU job per entry in `workers`, in a directory holding the plan and its
`blobs/`:

```sh
llm-cc compare worker --plan prep/plan.json --worker-id 0 \
  --cache s3://bucket/llm-cc --output-dir workers/0 --model /models/model.gguf
```

The worker takes its scoring settings from the plan and refuses a plan that
another llm-cc build prepared, a model with other weights, or a backend other
than the planned one. It loads the model once for every language, verifies each
blob against its Git object ID, stores each result as soon as it is ready and
always writes `worker-N.json`, with status `complete` only when every assigned
file was scored. The deadline defaults to 6600 seconds.

The CPU aggregation stage accepts every worker artifact produced:

```sh
llm-cc compare aggregate --plan prep/plan.json \
  --worker workers/0/worker-0.json --output-dir report
```

Each `--error MESSAGE` turns the report into a failure; without `--plan`,
`--identity FILE` names the pipeline the failure belongs to. The Python stages
that write failure reports run `$LLM_CC`, or `llm-cc` from `PATH`.

`llm-cc compare store get|put KEY --cache LOCATION` reads or writes one object
of a filesystem or `s3://bucket/prefix` store for operators. S3 requests are
signed with SigV4 from `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY` and
`AWS_SESSION_TOKEN`; `--store-options` may set `endpoint_url` and
`region_name`, which otherwise come from `AWS_ENDPOINT_URL[_S3]` and
`AWS_REGION`. Store options never carry credentials.

Every aggregation writes `report.json`, `report.md`, `comment.md`,
`publication.json`, `baseline.md` and `baseline.json`, including on validation
failure. An increased score is reported but does not make the command fail.
Missing or invalid analysis does. [SCHEMA.md](SCHEMA.md) describes the plan,
worker, result and report formats.

## What the report contains

Total LM-CC is the primary metric throughout the report: category summaries,
changed-file values and deltas, leading improvements/regressions, and file ranks.
Rankings sort by descending total LM-CC, with paths breaking ties. LM-CC/token
remains a separately labeled secondary metric; it can rise while total LM-CC
falls when a refactor removes proportionally more tokens than complexity.

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

`baseline.md` and `baseline.json` describe the run's head revision on its own:
category scores, the 50 worst-scoring files overall, the 20 worst per category,
and the unmeasured paths. On a pull-request run they describe the PR head. The
copies published from default-branch pushes and retained by the publisher's
`keep_default_branch_sets` are the repository baseline. Set
`report_links.baseline` in the BuildBuddy configuration to that published URL and
pull-request comments will link to it.

## Per-repository classification rules

`prepare` reads `.llm-cc/rules.json` from the **target** commit's tree, falling
back to the legacy `.llm-cc/comparison-rules.json`, with the same rules engine
as local `llm-cc` analysis; see
[selection and classification rules](../../README.md#selection-and-classification-rules).
A pull request cannot reclassify its own files, and rules that fail validation
fail the run instead of silently reverting to the defaults. A target without
that file uses `--default-rules`, the rules the host publishes, and otherwise
llm-cc's built-in rules. The report names the source it used.

Globs are anchored and segment-aware, like `.gitignore`: `*` and `?` never
cross `/`, and a whole-segment `**` matches zero or more directories.
`llm-cc rules explain PATH...` shows how a rules file classifies paths.
Accepted keys are `exclude`, `tests`, `tooling`, `extensions` and `paths`; glob
lists hold non-empty patterns of at most 256 characters, at most 512 patterns in
total including `paths`, and the document must stay under 64 KiB.

`paths` is an ordered list of `{"pattern": <glob>, "language": <supported>}`
objects that choose a language by location rather than by extension, for a
repository where the same extension means different languages in different
directories:

```json
{"paths": [{"pattern": "include/legacy/**/*.h", "language": "c"}]}
```

The first matching `paths` rule wins, then `extensions`, then the built-in
extension table. The resolved language is part of a file's result key, so the
same bytes under two differently overridden directories are scored separately.
Symlinks and submodules stay unscorable whatever rule matches them. See
[consumer/comparison-rules.json](consumer/comparison-rules.json) for a
per-repository example.

## Identity, fingerprint and cache

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

`discover` writes one with `head_sha`, `target_sha` and a null `base_sha`;
preparation fills in the resolved commits and their merge base, and refuses an
identity naming other commits. Fetch full history if merge-base resolution
fails. Comparisons read committed Git objects, including headers; they never
score working-tree changes or follow symlinks and submodules.

The fingerprint hashes three objects: the **scorer** (llm-cc version, stamped
commit or, for an unstamped build, the executable's SHA-256, backend
configuration, inference ABI and analysis version), the **model** (SHA-256 and
size) and the **scoring** settings (backend and GPU layers, context, batch,
entropy reduction, Flash Attention, K/V type and offload, hierarchy, tau and
alpha). A result's key hashes the file's Git blob ID, its language and the
fingerprint, so path, category, branch and application revision never change
it, and every llm-cc release starts a fresh cache. Identical contents in one
language share inference but each path counts toward reporting.

Oversized supported files remain unmeasured, and affected category/repository
scores and totals are unavailable. Files are never truncated. The displayed
repository/category headline is raw LM-CC, `sum(llm_cc)`, printed with one
decimal; `score` in `report.json` remains `sum(llm_cc) / sum(tokens)` and is
shown as the LM-CC/token delta. Zero-token scores and zero-baseline percentages
are unavailable.

The store holds results under `results/v2/`, native entropy entries under
`entropy/v2/` (so a new tau or alpha reuses inference), and each pipeline's
plan, blobs, worker artifacts and report under `pipelines/<digest>/`. Results
refresh after 20 days and expire after 30 by default; configure the bucket's
lifecycle accordingly. Corrupt or mismatched entries are misses; read,
authentication and transport errors fail the run. Preparation reads cached
results in parallel, bounded by `--cache-concurrency` (default 8, maximum 64);
the plan does not depend on the bound.

Filesystem writes use atomic replacement with owner/group permissions (`0660`)
restricted by the writer's umask/default ACL. For executor accounts sharing a
filesystem store, provision its root with their shared group and mode `2770`,
and run each stage with umask `0007` so new directories and objects remain
accessible to that group. An owner-only deployment can use umask `0077`.

## BuildBuddy and Bazzite

The [BuildBuddy adapter](buildbuddy.py) uses the
[remote-run API](https://www.buildbuddy.io/docs/enterprise-api/#run) to submit only
planned misses. Configure [buildbuddy.example.json](buildbuddy.example.json) at
`LLM_CC_COMPARISON_CONFIG` on the CPU coordinator: `llm_cc` is the pinned
executable that prepares, scores and aggregates, `scoring_args` its scoring
contract, and `execution_host` the bare host's GPU contract. Workers run on a
bare host (`execution_image: "none"`): a checksummed `execution_bundle` fetches
the plan and blobs, then runs `llm-cc compare worker --execution-host`.
`execution_commit` pins the repository BuildBuddy checks out. `report_links`
holds https URL templates published in pull-request comments; only
`{repository}`, `{target_sha}` and `{target_branch}` may appear, and their
values are URL-quoted. Worker secrets use named environment inputs sent as
sensitive remote headers. The coordinator discovers the current open PR and
actual target, cancels obsolete workers, and retains reports under a key
derived from the **parent invocation** identity. Default-branch pushes compare
the head to itself and populate the baseline cache. A branch with no current PR
skips inference.

The `Complexity comparison` BuildBuddy action runs on pull requests targeting
`main` and on `main` pushes. This repository runs the checkout's own coordinator
through [dogfood.sh](dogfood.sh), so pull requests exercise coordinator changes
before the host bundle is refreshed; consuming repositories call the published
launcher instead. The script builds the checkout's `//:llm-cc` and exports it
as `LLM_CC`, which renders reports unless the host configuration names its own
`llm_cc`. The coordinator derives its own head, branch and default branch
from the checkout, unwrapping BuildBuddy's synthetic merge commit to the actual
pull-request head. ci-toolkit consumes the completed BuildBuddy status and
publishes the generated table and report links using `.ci-toolkit.yml` from the
PR's target commit. The publication policy must land on the target branch before
a fresh PR comparison can publish automatically.

In this BuildBuddy flow, ci-toolkit owns comment markers, serialization,
ordering, and PR-state rechecks, and the coordinator never posts comments. See
[ci-toolkit.example.yml](ci-toolkit.example.yml) for the publication policy.
Other CI systems can use the native `publish` command instead; see
[CI_RECIPE.md](CI_RECIPE.md#7-publication-with-ordering-protection).

## CI recipe

[CI_RECIPE.md](CI_RECIPE.md) is the provider-neutral guide to running these
stages in CI: pinning, the model/scorer/coordinator image roles, the store
layout, strict workers, reporting and ordered publication, with an acceptance
checklist. Python commands (`python3 -m tools.comparison`, Python 3.11+ and
Git, or the matching Bazel launchers) complete the pipeline around
`llm-cc compare`:

- `discover` resolves the current pull request and its actual target, or skips
  a branch without one, and writes `identity.json` plus dotenv outputs.
- `ci capacity` caps `--max-workers` by the configured capacity; `ci
  gitlab-child` and `ci github-matrix` turn a plan into zero to four GPU jobs: a
  GitLab child pipeline or a GitHub Actions matrix.
- `store-report` stores a report under the pipeline that produced it.
- `publish` validates a stored report and maintains one ordered comment per
  pull request, re-checking the pull request before it writes.

Ready-made [GitLab and GitHub Actions templates](recipe/README.md) and
Containerfiles for the three image roles live under [recipe/](recipe). A
consuming module can run `@llm_cc//tools/comparison:discover` and the other
launchers, including when its `bazel_dep` uses a different `repo_name`.

## Adopting in another repository

Copy the two templates in [consumer/](consumer) into the repository, replace
`OWNER/REPO` in the BuildBuddy action, and merge the ci-toolkit automation on the
default branch first. See [consumer/README.md](consumer/README.md) for the
prerequisites, the shared-store trust note, and the `GITHUB_TOKEN` guidance.
Execution always happens on the Bazzite host through
`/var/lib/llm-cc/bin/llm-cc-coordinate`, which pins the verified package and its
immutable configuration generation.

Local acceptance is `bazel test //:unit //tools/comparison:comparison_test` and
`BACKEND=cpu tools/check_consumer.sh --tests-only`. The comparison tests run
`llm-cc-compare-fake`, llm-cc with a deterministic scorer, through the real
stages: cold, warm and incremental runs, one or four workers, and zero remote
submissions on complete cache hits. Local correctness does not establish GPU
memory fit or live publication acceptance. See [VALIDATION.md](VALIDATION.md)
for executed checks and remaining live acceptance.

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
backend, without asserting that a particular scorer build has been validated.
The matching `gpu-resource` value identifies one physical device across both
executors. With `--execution-host`, `llm-cc compare worker` verifies the actual
PCI/KFD device, architecture, VRAM, access permissions and runtime hashes
before scoring, selects the discrete GPU by `ROCR_VISIBLE_DEVICES=GPU-<uuid>`,
and holds the host-wide `flock` on `/var/lib/llm-cc/locks/<resource_id>.lock`
while it scores. Both executors use the same root-owned lock inode; setup never
replaces it, and the worker never creates it. Lock waiting counts against the
worker deadline. Other GPU applications must cooperate with this lock to
participate in serialization.

Continue selecting the executor pool explicitly. BuildBuddy's
[`debug-executor-labels` selector](https://github.com/buildbuddy-io/buildbuddy/blob/master/enterprise/server/scheduling/scheduler_server/scheduler_server.go)
uses best-effort routing and can fall back when labels do not match; it is not
a strict GPU eligibility or concurrency constraint.

The Bazzite scoring contract uses ROCm, context **32768**, batch **256**, Flash
Attention on, **Q8_0** K/V with offload, device entropy reduction, structural
hierarchy, tau 0.67 and alpha 0.8, with the default **64 KiB** file limit. The
host configuration uses one worker because the two builders share one Radeon.
The CPU coordinator uses `linux-amd64-rocm` and the existing host filesystem
store. Follow [Bazzite setup](BAZZITE.md) to install the verified assets and
publish an atomic configuration generation.
