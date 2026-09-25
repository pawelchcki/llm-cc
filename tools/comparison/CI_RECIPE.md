# CI recipe: cached GPU scoring and pull-request reports

This guide packages the advisory pull-request complexity pipeline from
[issue #38](https://github.com/pawelchcki/llm-cc/issues/38) so another project
can reproduce it. CPU jobs discover the pull request, inventory both revisions
and check the result cache. GPU jobs score only uncached contents, so a fully
cached comparison schedules **zero GPUs**. One serialized publisher maintains a
single comment per pull request.

The comparison itself is `llm-cc compare prepare`, `worker` and `aggregate`;
discovery, job generation and publication are `python3 -m tools.comparison`
commands (or the matching `@llm_cc//tools/comparison:<stage>` Bazel
launchers), so the same contracts run under any CI system. The [recipe templates](recipe/README.md) wire them into a
GitLab parent/child pipeline and a GitHub Actions workflow. The dogfood
BuildBuddy flow in [README.md](README.md) uses the same stages with its own
transport and the ci-toolkit publisher, which remains a supported alternative.

## Pipeline

```text
images (when pins change)   build.sh: model -> scorer -> coordinator (scoring.args)
            |
CPU   discover             default branch: baseline; branch without PR: skip
CPU   llm-cc compare prepare   inventory base/head, check cache, plan misses
CPU   ci gitlab-child | ci github-matrix
            |
       cache misses?
       /          \
     none         1..4 single-GPU workers (unique contents, balanced by bytes)
       \          /
CPU   aggregate (always) -> store-report   pipelines/<digest>/report.json ...
            |
CPU   publish (always, serialized per branch): order -> re-check PR -> one comment
```

| Stage | Command | Runs on | Produces |
|---|---|---|---|
| Discover | `discover` | CPU, read-only GitHub token | `identity.json`, or `skipped.json`; `COMPARISON_*` dotenv |
| Prepare | `llm-cc compare prepare` | CPU, store read | `plan.json` and `blobs/` for misses |
| Plan jobs | `ci gitlab-child` / `ci github-matrix` | CPU | child pipeline YAML / matrix outputs |
| Score | `llm-cc compare worker` | one GPU each | `worker-<id>.json`; results and native entries in the store |
| Aggregate | `llm-cc compare aggregate`, `store-report` | CPU, store write | reports under the pipeline prefix |
| Publish | `publish` | CPU, comment token | one comment and a publication marker |

Default-branch pushes compare the head with itself, which seeds and refreshes the
baseline cache; they store a report but never comment. Any other branch
resolves its current open pull request and that PR's **actual** target, which
need not be the default branch. A branch without one skips scoring. Jobs are
interruptible and advisory: an increased score never fails a job, and the
application's own build and tests stay authoritative.

## 1. Pin the tool, backend, model and scoring contract

Images are built from a clean llm-cc checkout at one full commit;
[`build.sh`](recipe/images/build.sh) refuses anything else, and builds the
scorer from a `git archive` of that commit, so ignored local files (models,
caches, `bazel-*` trees) never reach the build. The scorer build passes
`--//:source_commit` and `--//:source_version` explicitly, so llm-cc's identity
names its own revision and never the application's.
Remote build caching is optional and arrives only as a build secret; local
builds need no organizational credentials. Projects that consume llm-cc as a
Bazel module instead should follow the [consumer example](../../examples/consumer/README.md);
`tools/check_consumer.sh` checks installation and every comparison launcher's
runfile resolution from a consumer whose `bazel_dep` uses another `repo_name`.

The scoring contract is explicit: `build.sh` writes the `SCORING` settings and
the model pin into `scoring.args`, one argument per line, and the planning
limits into `planning.args`, and bakes both into the coordinator.
`llm-cc compare prepare` and `llm-cc compare identity` read them as response
files (`@FILE`). The issue's A10-class CUDA contract corresponds to these
values:

| Input | Value | Source |
|---|---|---|
| llm-cc revision | the commit you pin (the issue used `0429943fde3856d16dd98b037bb40bb1f68beac6`) | `LLM_CC_COMMIT` |
| Inference ABI, analysis version | reported by the pinned llm-cc | the scorer's own build |
| Model | `DeepSeek-Coder-V2-Lite-Base-Q6_K`, SHA-256 `5a2e25280075d769abdb111de8211d9d3367f2ae0d0e6166a288ee6e8ed0345d`, 14,066,972,416 bytes | `MODEL_URL`, `MODEL_SHA256`, `MODEL_BYTES` |
| GPU target | `compute_86:sm_86` | `CUDA_ARCHS` |
| Scoring | `--backend cuda --gpu-layers -1 --context 131072 --batch-size 256 --flash-attn on --kv-cache-type q8_0 --kv-offload on --entropy-reduction device --hierarchy structural --tau 0.67 --alpha 0.8` | `SCORING` |
| Maximum scored file | `--max-file-bytes 49152` (48 KiB) | `PLANNING` |

`build.sh` also writes `identity.json`, the output of
`llm-cc compare identity @scoring.args`, for review:

```text
fingerprint = SHA256("llm-cc-compare-fingerprint-v2\0" + canonical_json({scorer, model, scoring}))
```

`scorer` is llm-cc's version, stamped commit, backend configuration, inference
ABI and analysis version; `model` its SHA-256 and size; `scoring` every setting
above except the file limit. Changing the model, the llm-cc build or any scoring
setting therefore invalidates every prior result; a different GPU layer count or
backend is a different identity. `auto` settings are refused, so the contract
never depends on the machine that prepares it. The context is an upper bound
with demand-sized allocation, not a promise that every input fits; the file
limit is a conservative eligibility policy. Larger supported files stay in the
inventory as unmeasured coverage and are never truncated. A lower-precision K/V
contract is a separate experiment with a separate fingerprint.

## 2. Three image roles

| Role | Containerfile | Contents | Tag (lookup hint) |
|---|---|---|---|
| Model | [model.Containerfile](recipe/images/model.Containerfile) | `/models/model.gguf` on `scratch`, size and SHA-256 verified | the weights' SHA-256 |
| Scorer | [scorer.Containerfile](recipe/images/scorer.Containerfile) | the model image by digest, plus one layer: `/opt/llm-cc` and user 10001; no Python | hash of commit, version, archs, model digest, base images, Bazelisk pin, Containerfile |
| Coordinator | [coordinator.Containerfile](recipe/images/coordinator.Containerfile) | Git, Python with boto3, the provider glue, the scorer's own `llm-cc`, `scoring.args`, `planning.args`, default `rules.json`, user 10001 | hash of commit, scorer digest, scoring and planning arguments, Containerfile |

The scorer's final stage starts `FROM` the model image by digest and adds the
whole runtime as one layer above it, so every scorer rebuild on the same weights
reuses the multi-gigabyte model layer by reference. A fresh GPU node still has
to pull that layer once; this does not remove cold model distribution. The
build rejects a `MODEL_IMAGE` that is not pinned by digest, mounts an optional
`bazelrc` build secret for remote-cache credentials instead of taking them as
build arguments, and fails early if the installed llm-cc cannot state its own
identity. llm-cc reads plans, stores results in S3 and writes worker artifacts
itself, so the GPU image carries neither Python nor an S3 SDK.

The coordinator copies `llm-cc` from the pushed scorer image, so preparation,
scoring and aggregation run one build; a worker refuses a plan that any other
build prepared. The coordinator also records the scorer image digest as
`LLM_CC_SCORER_IMAGE`, which the CI adapters use for GPU jobs unless the CI
configuration names `images.scorer`. One coordinator digest thus pins the
glue, llm-cc, the scoring contract and the exact scorer every GPU job runs.

`build.sh` treats content-derived tags only as lookup hints. A tag found in the
registry is reused after its labels match the producer's source identity
(`org.opencontainers.image.revision`, or the model digest and size), the scorer
and coordinator also record the tag they were built for
(`io.llm-cc.input-key`), and its signature verifies with `COSIGN_PUBLIC_KEY`;
`TRUST_REGISTRY=1` accepts a registry only CI can push to. A fresh build uses the digest `podman push --digestfile` reports,
never a tag resolved afterwards, and signs it when `COSIGN_PRIVATE_KEY` is set.
The output is `images.env` with `MODEL_IMAGE`, `SCORER_IMAGE` and
`COORDINATOR_IMAGE`, all `name@sha256:<digest>`, plus `identity.json` for
review. The templates run it on demand (GitLab: `COMPARISON_BUILD_IMAGES=1`);
pin the printed coordinator digest afterwards.

## 3. Inventory Git objects and plan only missing work

`discover` resolves the identity with a read-only token. Default-branch pushes
must still be the branch head. Branch pushes list the open pull requests for the
exact head commit, keep only those whose head branch matches and whose base
repository is this one, and reject more than one as ambiguous. With `--repo` it
fetches the target commit (falling back to the target branch for mirrors that
refuse object fetches) and unshallows the checkout. It writes `identity.json` or
`skipped.json`, and appends `COMPARISON_SKIP`, `COMPARISON_PR_NUMBER` and
`COMPARISON_TARGET_SHA` to a dotenv file usable as a GitLab dotenv report or
`$GITHUB_OUTPUT`.

`llm-cc compare prepare` computes `base = merge-base(head, actual_target)` and
lists both trees with `git ls-tree`. It accepts tracked regular blobs, headers
included, never follows symlinks or submodules, and keeps a reason
(`unsupported`, `excluded`, `oversized`, `symlink`, `submodule`) for every
unmeasured path. Classification rules come from `.llm-cc/rules.json` (or the
legacy `.llm-cc/comparison-rules.json`) in the **target** commit, so a pull
request cannot reclassify itself; otherwise from the coordinator's
`--default-rules` ([example](recipe/config/rules.example.json)). Tests take
precedence over tooling and runtime.

```text
result_key = SHA256("llm-cc-compare-result-v2\0" + blob_id + "\0" + language + "\0" + fingerprint)
```

`blob_id` is the file's Git object ID, which `ls-tree` provides for free. The
key excludes path, category, branch and revision, so identical contents in one
language share inference across copies, renames, revisions and branches,
while aggregation still counts every path. Preparation reads unique keys with
bounded concurrency, reads the bytes of misses only, sorts misses by descending
size with the key as tie-breaker, and assigns each to the least-loaded of
`min(--max-workers, misses)` workers. `plan.json` carries the identity, the
scorer, model and scoring, the fingerprint, inventories, changes, validated
hits, assignments and `blobs/<blob_id>` for misses, so no GPU job touches Git.
In the GitLab template `ci capacity` caps `--max-workers` by the configuration's
`max_workers`, which `ci gitlab-child` enforces.

`ci gitlab-child` turns the plan into a child pipeline, written as JSON, which
is valid YAML: one GPU job per worker (the scorer image by digest,
`interruptible`, 120 minute timeout, `GIT_STRATEGY: none`) running
`llm-cc compare worker`, and an aggregation job with `when: always`. Every
worker writes `comparison/workers/<id>/` and preparation artifacts contain no
worker files, so downloading them into the aggregation job can never overwrite
a completed result. A fully cached plan yields only the aggregation job and
needs no scorer image. `ci github-matrix` prints `matrix`, `has_workers` and
`scorer_image` outputs; the GPU job is skipped when `has_workers` is false.

## 4. Two cache layers with explicit provenance

Stores share one interface over a filesystem directory (atomic replacement) or
`s3://bucket/prefix`; llm-cc reads and writes the caches itself, and the Python
glue only the pipeline and publication objects:

| Object key | Contents |
|---|---|
| `results/v2/<result_key>.json` | `{schema_version, stored_at, sha256, result}` with `result = {key, blob_id, language, fingerprint, llm_cc, token_count, high_entropy_tokens, entropy_sum, total_branch, total_comp_level}` |
| `entropy/v2/<entry key>.cbor` | one native entropy entry, keyed by the source bytes, model and inference settings |
| `pipelines/<SHA256([repository, pipeline_id])>/` | `report.json`, `report.md`, `comment.md`, `publication.json`, `baseline.md`, `baseline.json` (BuildBuddy transport also keeps its plan, blobs and worker artifacts here) |
| `publications/<SHA256([repository, pr_number])>.json` | publication marker `{schema_version, ordinal, pipeline_id, head_sha, state, comment_id}` |

This differs from the issue's sketch in three ways: the result envelope checks a
digest over the canonical result rather than wrapping base64 data; pipeline
prefixes hash `[repository, pipeline_id]`, because pipeline IDs are opaque input
and never path components; and markers are keyed by pull request rather than
branch, because one comment belongs to one pull request.

Corrupt, mismatched, expired or future-dated entries are misses; read, write,
permission and network errors fail the stage. Checksums detect corruption;
access control decides who may publish results. Validated results older than
`--refresh-days` (20) are rewritten, and `--expire-days` (30) rejects older
ones. Configure the bucket to expire `results/` and `entropy/` 30 days after the
last write; expire `pipelines/` after your artifact retention. Markers are tiny;
if one expires, the comment's embedded ordinal still blocks older pipelines.
Result and entropy writes are idempotent and last-writer-wins; llm-cc uses only
GET and PUT, signed with SigV4 from the `AWS_*` environment.

Markers use conditional writes. The filesystem store serializes writers with a
per-key `flock` under `.locks/` and versions objects by SHA-256, so it needs a
local or lock-capable shared filesystem. The S3 store uses `If-None-Match: *` to
create and `If-Match: <ETag>` to replace, which needs a late-2024 or newer boto3
and an S3-compatible service that supports conditional writes; a service that
rejects them fails publication loudly rather than racing.

Native entropy entries are shared through the store per source file, so a new
tau or alpha, which changes every result key, still reuses inference. A corrupt
entry is rescored rather than trusted.

## 5. Bounded workers and strict results

Each worker gets one GPU, checks that the plan was prepared by its own llm-cc
build, that the model's digest and size match the plan's, and that it would
score with the planned backend, then loads the model once for every language.
Each blob is verified against its Git object ID before scoring; a mismatching
blob fails that file, not the worker. Each result is stored as soon as it is
ready, so a worker cut short keeps the work it finished. The deadline is 110
minutes (`--deadline-seconds 6600`) inside a 120-minute job; the worker cannot
stop in the middle of a file, so the job timeout is the hard stop. The worker
always writes `worker-<id>.json`, with status `complete` only when every
assigned file was scored, and the errors otherwise.

Aggregation always runs, and accepts a worker only when its pipeline identity,
fingerprint, completion status and exact result set match the plan; a missing
worker artifact is reported by ID. Scorer images run as user 10001. GitLab's
Docker executor extracts artifacts world-writable by default (keep
`FF_DISABLE_UMASK_FOR_DOCKER_EXECUTOR` off); on Kubernetes set the pod's
`fsGroup`; the GitHub workflow runs containers as the runner's own user.

## 6. Reporting

For each category and revision the reported score is token-weighted:

```text
category_score = sum(file.llm_cc) / sum(file.token_count)
```

The headline is raw LM-CC, `sum(llm_cc)`, with token totals, measured and known
paths and bytes, and absolute and percentage changes. Duplicate contents are
scored once and counted once per path. A category with any unmeasured supported
file withholds its totals and score; zero-token scores and zero-baseline
percentages are unavailable. Additions and deletions have an absent side; renames
and category moves keep both paths and categories. `comment.md` stays within
24 KiB and renders every path inside a balanced code span; `report.md` and
`report.json` carry full inventories, exclusions, failures and results. A failed
analysis still writes every report file with status `failed`. See
[README.md](README.md#what-the-report-contains) and [SCHEMA.md](SCHEMA.md).

## 7. Publication with ordering protection

`store-report` stores a report under the prefix of the identity inside it. A
child pipeline's aggregation therefore stores under the **parent** pipeline ID
carried in the plan, which is where the parent's publisher looks. `publish` then:

1. Loads `publication.json` and `comment.md` for the trusted `--repository` and
   `--pipeline-id`, verifies the comment's SHA-256 and size, and checks that the
   report's repository, pipeline, head and, with `--identity`, target and PR
   match. Without a stored report, or with `--failure MESSAGE`, it builds and
   stores a failure report from the discovery identity, so failed image,
   preparation or scoring stages still reach the pull request.
2. Stops after storing for a default-branch run.
3. Reads the pull request's marker. A larger stored ordinal means a newer
   pipeline has reserved or completed publication: the older one exits as
   `stale`. Otherwise it conditionally writes a `reserved` record, re-reading on
   conflict. Ordinals are `CI_PIPELINE_IID` on GitLab and `github.run_id` on
   GitHub, which increase with pipeline creation and, unlike
   `github.run_number`, do not restart when the workflow file is renamed.
4. Re-reads the pull request: open state, head, target branch and target commit.
   A closed, updated or retargeted pull request is marked `suppressed`.
5. Updates the single comment that starts with `<!-- llm-cc-comparison -->` and
   was written by `--comment-author` (default: the token's login), or creates
   it. The next line records `pipeline=<id> ordinal=<n>`; a comment with a
   larger ordinal is never overwritten, even if the marker was lost. Comments by
   anyone else carrying the marker are ignored.
6. Records `published` with the comment ID.

Reserving before the external update means a lost API response cannot let an
older pipeline overwrite a newer comment: its retry adopts the comment it
created. Publication must be serialized per branch (`resource_group` on
GitLab, `concurrency` on GitHub): two publishers writing at the same moment
can still interleave their comment updates. Beyond that, the conditional
marker and the embedded ordinal reject an older pipeline, such as a delayed or
retried job, that publishes after a newer one has reserved. The re-check is not
atomic with pull-request changes: a push or retarget between the re-check and
the comment update leaves one outdated comment until the next pipeline
publishes. Stale and suppressed outcomes exit 0 with the reason printed;
validation, storage and API errors exit non-zero.

## Credentials

| Credential | Used by | Scope |
|---|---|---|
| Discovery token (`GITHUB_READ_TOKEN`, Actions job token) | discover, late discovery in publish | Pull requests and Contents read |
| Comment token (`GITHUB_COMMENT_TOKEN`, Actions job token) | publish only | Pull requests write; GitLab environment scope `llm-cc-publisher` |
| Store credentials | prepare, workers, aggregate, publish | the bucket prefix only; never in `store_options` or artifacts |
| Registry push and signing keys | image builds only | the image repositories |
| Remote build cache | scorer builds only | `bazelrc` build secret, never a layer or build argument |

Prefer short-lived GitHub App installation tokens minted per pipeline; revoke
them after use with `DELETE /installation/token`. Whoever can push a branch
controls that branch's pipeline definition, so the comment token is only as
protected as push access: give it to a dedicated account or app limited to
commenting on this repository. Neither template runs for fork pushes.

## Acceptance checklist

Local checks run with `bazel test //:unit //tools/comparison:comparison_test`.
The C++ tests cover the comparison core; the Python tests run the real stages
through `llm-cc-compare-fake`, llm-cc with a deterministic scorer.

| Issue checkbox | Evidence | Status |
|---|---|---|
| Cold fixture run produces a complete comparison | `test_recipe`, `compare_worker_test` | local |
| One- and four-worker runs agree | `compare_worker_test`, `compare_prepare_test` (stable partitioning) | local |
| Fully cached run schedules zero GPUs within budget | `test_recipe` (warm push: aggregation-only child, 60 s budget), `test_ci`, `test_buildbuddy` | local; object-store latency is measured separately in [VALIDATION.md](VALIDATION.md) |
| Renames, duplicates, headers, category moves, exclusions, oversized files, absent sides | `test_recipe`, `compare_prepare_test` | local |
| Settings changes invalidate; corrupt caches miss; storage failures surface | `compare_identity_test`, `compare_result_cache_test`, `compare_store_test` | local |
| Wrong builds, models and blobs never score; deadlines still report | `compare_worker_test`, `test_buildbuddy` | local |
| Shared native entropy entries; non-root artifact extraction | `compare_worker_test`; ownership notes above | local; non-root extraction is a live step |
| Cold/warm image builds and layer reuse | `test_recipe` image-build tests with a fake engine | builds and layer reuse are live steps |
| Default-branch seeding, one reused comment over two PR updates, delayed older pipelines, retargeting, failure reports | `test_recipe`, `test_publish` against in-memory GitHub | local; a live GitLab or GitHub rollout is not verified here |
