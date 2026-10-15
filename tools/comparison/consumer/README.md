# Adopting the complexity comparison in another repository

Every pull request gets an upserted `llm-cc comparison` comment with per-category
scores, a per-file table of the files the PR touches, and the worst-scoring files
on the target branch. Default-branch pushes publish a `baseline.md` ranking of the
whole repository. All execution happens on the shared Bazzite ROCm host, so the
consuming repository only supplies configuration.

## Prerequisites

- The BuildBuddy GitHub app is installed on the repository, and the repository's
  BuildBuddy group has access to the `linux-amd64-kvm` pool, where `ccd1`
  advertises the `bazzite-host` custom resource.
- The ci-toolkit GitHub app is installed on the repository.
- `/var/lib/llm-cc/bin/llm-cc-coordinate` exists on the host. Re-run
  `python3 -m tools.comparison.setup_bazzite` after any llm-cc or comparison
  source change.
- The publication policy is merged on the **default branch first**. ci-toolkit
  reads `.ci-toolkit.yml` from a pull request's target commit, so a policy that
  exists only on the PR branch publishes nothing.

## Files to copy

1. [`buildbuddy.yaml`](buildbuddy.yaml) into the repository root. Replace
   `OWNER/REPO` with the repository's `owner/name`, and replace the two `main`
   branch filters and the `--default-branch main` argument with the
   repository's default branch — left as `main` on a `master` or `trunk`
   repository the action never triggers, so no status is published and nothing
   downstream reports an error. Keep the action name
   `Complexity comparison`; the ci-toolkit trigger matches that commit-status
   context exactly.
2. [`.ci-toolkit.yml`](.ci-toolkit.yml) into the repository root, keeping its
   `api_version` line, or merge only its `automations` entry into an existing
   policy that already declares one.

Add `.llm-cc/rules.json`, based on
[`comparison-rules.json`](comparison-rules.json), to classify paths for the
language at hand. This is strongly recommended rather than optional: a
repository without that file is scored with llm-cc's built-in rules. They map
`.h` to C, treat only a few top-level directories (`tools/**`, `examples/**`,
`scripts/**`, `benchmarks/**`) as tooling, and exclude common vendored and
generated trees, so a C++ project or a differently laid out repository gets
misclassified languages and category rankings rather than an error. Check a
rules file with `llm-cc rules explain PATH...` before committing it.

Rules are read from the **target** commit's tree, so a pull request cannot
reclassify its own files. Invalid rules fail the run rather than silently
falling back to the defaults. Accepted keys are `exclude`, `tests`,
`tooling`, `extensions`, and `paths`. Globs work like `.gitignore`: `*` never
crosses `/`, and `**` matches any number of directories.

`paths` chooses a language by location, for a repository where one extension
means different languages in different directories:

```json
{"paths": [{"pattern": "include/legacy/**/*.h", "language": "c"}]}
```

The first matching `paths` entry wins, then `extensions`, then the built-in
extension table.

## Private repositories and GitHub quota

Public repositories need no token. Anonymous GitHub REST reads share a
60-per-hour quota by source IP with everything else on the host, so without a
token the coordinator reads Git refs instead, which use no REST quota:

- BuildBuddy names the pull request (`GIT_PR_NUMBER`, `GIT_BASE_BRANCH`,
  `GIT_BRANCH`, `GIT_COMMIT`). The coordinator checks that the repository's
  branch and `refs/pull/N/head` both point at the head, and takes the target
  from GitHub's test merge, `refs/pull/N/merge`, whose first parent is the
  recorded base that GitHub's API reports as `base.sha`.
- A default-branch push checks `refs/heads/<default>` with `git ls-remote`.
- Freshness checks while GPU workers run use `git ls-remote` as well. GitHub
  drops a PR's test merge when the PR closes and recomputes it when the PR is
  retargeted, so an unchanged merge also shows the PR is open on the same base.

The REST API is used only when Git refs cannot answer: GitHub has no test
merge for the head, for example while a pull request has conflicts (one read,
then freshness reads every three minutes), the test merge disappears during a
run, or the run has no BuildBuddy pull-request event to name the pull request.
A refused quota is retried after the wait GitHub names, sleeping at most
`github_rate_limit_wait_seconds` (default 300) per run, so the 130-minute action
still fits the coordinator's two-hour deadline. A longer wait, such as an
exhausted anonymous hour, defers the run: it writes `skipped.json` with
`"retryable": true`, publishes no failed comparison, and exits with status 75.
Results already scored stay cached, so a re-run after the reset is quick.

Private repositories need a `GITHUB_TOKEN` BuildBuddy secret for the
repository's group, which also switches discovery and freshness checks to the
REST API.

## Shared store

Comparisons share one host filesystem cache. Result keys are the Git blob ID,
language and scoring fingerprint only, and transport paths hash
`[repository, pipeline_id]`, so repositories neither collide nor learn each
other's paths. Uploaded blobs are
host-local and readable by both executor users, so only run this on repositories
whose source those users may read.

## What lands where

- The pull-request comment comes from `comment.md`, bounded to 24 KiB.
- `report.md` and `report.json` hold the complete per-file detail.
- `baseline.md` and `baseline.json` hold the ranking for the run's head commit.
  The default-branch copies, retained by `keep_default_branch_sets: 5`, are the
  repository baseline. Point `report_links.baseline` in the host configuration at
  that published URL to have pull-request comments link to it.
