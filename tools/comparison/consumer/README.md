# Adopting the complexity comparison in another repository

Every pull request gets an upserted `llm-cc comparison` comment with per-category
scores, a per-file table of the files the PR touches, and the worst-scoring files
on the target branch. Default-branch pushes publish a `baseline.md` ranking of the
whole repository. All execution happens on the shared Bazzite ROCm host, so the
consuming repository only supplies configuration.

## Prerequisites

- The BuildBuddy GitHub app is installed on the repository, and the repository's
  BuildBuddy group has access to the `linux-amd64-rocm` pool.
- The ci-toolkit GitHub app is installed on the repository.
- `/var/lib/llm-cc/bin/llm-cc-coordinate` exists on the host. Re-run
  `python3 -m tools.comparison.setup_bazzite` after any comparison source change.
- The publication policy is merged on the **default branch first**. ci-toolkit
  reads `.ci-toolkit.yml` from a pull request's target commit, so a policy that
  exists only on the PR branch publishes nothing.

## Files to copy

1. [`buildbuddy.yaml`](buildbuddy.yaml) into the repository root. Replace
   `OWNER/REPO` with the repository's `owner/name`. Keep the action name
   `Complexity comparison`; the ci-toolkit trigger matches that commit-status
   context exactly.
2. [`.ci-toolkit.yml`](.ci-toolkit.yml) into the repository root, or merge its
   `automations` entry into an existing policy.

Optionally add `.llm-cc/comparison-rules.json`, based on
[`comparison-rules.json`](comparison-rules.json), to classify paths for the
language at hand. Rules are read from the **target** commit's tree, so a pull
request cannot reclassify its own files. Invalid rules fail the run rather than
silently falling back to the host defaults. Accepted keys are `exclude`, `tests`,
`tooling`, and `extensions`.

## Private repositories and GitHub quota

The coordinator discovers the open pull request through the GitHub REST API.
Anonymous reads share a 60-per-hour quota by source IP, which the host also uses
for every other repository. Configure a `GITHUB_TOKEN` BuildBuddy secret for the
repository's group. A token is required for private repositories and strongly
recommended for public ones.

## Shared store

Comparisons share one host filesystem cache. Result keys are content and scoring
fingerprint only, and transport paths hash `[repository, pipeline_id]`, so
repositories neither collide nor learn each other's paths. Uploaded blobs are
host-local and readable by both executor users, so only run this on repositories
whose source those users may read.

## What lands where

- The pull-request comment comes from `comment.md`, bounded to 24 KiB.
- `report.md` and `report.json` hold the complete per-file detail.
- `baseline.md` and `baseline.json` hold the ranking for the run's head commit.
  The default-branch copies, retained by `keep_default_branch_sets: 5`, are the
  repository baseline. Point `report_links.baseline` in the host configuration at
  that published URL to have pull-request comments link to it.
