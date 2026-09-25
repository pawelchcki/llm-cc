# Bazzite host setup

The two Bazzite BuildBuddy executors share one Radeon RX 7900 XTX. The `ccd1`
executor uses the dedicated `linux-amd64-rocm` pool for bare-host dogfooding;
`ccd0` retains the general `linux-amd64-kvm` pool. Use the ROCm scoring contract
and one worker. Both the CPU coordinator and GPU child target the dedicated pool, where
the executor's 12 logical CPUs and 60 GiB memory allow both jobs to run together
at their requested one compute unit each. The worker also holds a host-wide file
lock so independent invocations cannot score concurrently. The lock covers this
comparison pipeline; unrelated GPU applications must follow the same convention
to participate.

Install an llm-cc ROCm build in `/var/lib/llm-cc/scorers/<version>-rocm`,
retain the existing checksum-pinned model, and supply an existing shared
filesystem cache directory. The coordinator, the worker and the report all run
that one executable; a worker refuses a plan from any other build, so a new
llm-cc means a new setup run.

Describe the GPU in an execution-host JSON file, the contract
`llm-cc compare worker --execution-host` verifies before every run:

```json
{
  "gpu_vendor": "amd",
  "gpu_pci_address": "0000:03:00.0",
  "gpu_arch": "gfx1100",
  "gpu_vram_bytes_min": 25753026560,
  "resource_id": "bazzite-radeon-0",
  "runtime_files": {"/usr/lib64/libamdhip64.so.6": "<sha256>"}
}
```

It names the physical GPU's PCI address, architecture and minimum VRAM, the
shared lock resource, and the SHA-256 of each host ROCm runtime library the
scorer loads.

For different executor accounts, provision the cache root with their shared group
and mode `2770`, and configure both executor services with umask `0007`. Cache
objects follow the process umask/default ACL; existing objects must also be
readable by that group. Host setup does not change cache permissions.

Publish a reproducible package and configuration from the implementation checkout:

```sh
sudo python3 -m tools.comparison.setup_bazzite \
  --installed-root /var/lib/llm-cc/scorers/<version>-rocm \
  --model /var/home/pawel/.cache/llm-cc/models/DeepSeek-Coder-V2-Lite-Base-Q6_K.gguf \
  --execution-host /path/to/execution-host.json \
  --cache /var/lib/llm-cc/cache \
  --execution-commit <full commit of this checkout> \
  --lock-group pawel
```

Use the actual executor user's group for `--lock-group`. The dogfood ROCm
scoring contract is built in (context 32768, batch 256, Flash Attention on,
Q8_0 K/V with offload, device entropy reduction, structural hierarchy, tau 0.67,
alpha 0.8); `--scoring-args FILE` replaces it, one argument per line. The helper
checks the execution host and every runtime file digest, runs
`llm-cc compare identity --model MODEL` once to hash the model and validate the
scoring contract, then creates:

- `packages/<sha256>.zip`: a deterministic snapshot containing only the Python
  glue. It includes working-tree implementation changes, so remote jobs do not
  require an uncommitted package to exist on GitHub.
- `configurations/<identity>/`: `scoring.args` (the scoring contract plus
  `--model-sha256` and `--model-bytes`, so coordinators never rehash the
  model), `execution-host.json`, `identity.json` (scorer, model, scoring and
  fingerprint) and the nonsecret comparison settings.
- `comparison.json`: atomically published current settings.
- `locks/bazzite-radeon-0.lock`: a root-owned group-writable persistent lock beneath
  root-owned directories. Keep this inode in place while workers may be running.
- `bin/llm-cc-coordinate`: a mode 0755 launcher that execs the verified package
  with the immutable generation's `comparison.json`. It is published last, after
  the package and the generation exist, and forwards its own arguments. Consuming
  repositories run `/var/lib/llm-cc/bin/llm-cc-coordinate --repository OWNER/REPO`
  as their only BuildBuddy step, so they never need the comparison source.

Pass `--report-links` a JSON object of https URL templates to publish links in
pull-request comments, for example
`--report-links '{"baseline": "https://ci.example/{repository}/baseline.md"}'`.
Only `{repository}`, `{target_sha}` and `{target_branch}` may appear.

Configuration points at existing host scorer/model/cache paths. The helper never
downloads model weights, provisions an external store, or copies credentials. The
cache and model must be accessible to both executor users. Package and configuration
files are readable by both workers; their parent directories should remain
root-owned. Re-run setup whenever llm-cc, the comparison source, the scoring
contract, the host runtime or the report links change, including after merging a
comparison change to the default branch; the launcher and its pinned generation
only move when setup runs. The source ZIP hash pins the executed Python glue;
the Git execution commit is repository metadata. The result cache identity is
the fingerprint in `identity.json`.

Workers use the ZIP bootstrap's checksum verification before loading any bundled
Python code, fetch the plan and blobs, and run
`llm-cc compare worker --execution-host`. Device identity and runtime checksums
must validate before inference, and the worker holds the lock while it scores;
label routing remains a best-effort scheduling preference. The dedicated pool
keeps host-local packages, model, and cache accessible to both stages. The generic
pool also contains other machines, so GPU labels alone cannot make it suitable.
A wrong host fails with an explicit worker artifact. Only actual cache misses
create GPU worker requests.

To submit a manual **CPU coordinator** from the implementation checkout, export
`BUILDBUDDY_API_KEY` through your existing credential
mechanism, then run:

```sh
python3 -m tools.comparison.submit_bazzite submit \
  --config /var/lib/llm-cc/comparison.json \
  --repository pawelchcki/llm-cc
```

`--head`, `--branch` and `--default-branch` are optional; without them the
command derives the head from `GIT_COMMIT`, `COMMIT_SHA`, the runner's build
metadata or `git rev-parse HEAD`, unwraps a BuildBuddy synthetic merge commit to
its first parent, reads the branch from `GIT_BRANCH`, the current checkout or the
runner's `BRANCH_NAME`, and takes the default branch from
`GIT_REPO_DEFAULT_BRANCH`, falling back to `main`. `--repository` is required.

The command prints the new parent invocation URL. The local API key authenticates
the submission; local credentials are never copied into the job request.
BuildBuddy's [Run implementation](https://github.com/buildbuddy-io/buildbuddy/blob/master/enterprise/server/hostedrunner/hostedrunner.go)
injects its own group credential into the runner. Public GitHub PR discovery needs
no token. For private repositories, configure access through trusted BuildBuddy
runner or secret settings. The command checks out the requested application
revision into the runner's own checkout. Do not point remote jobs at another user's
working tree; Git correctly rejects that ownership mismatch. The parent verifies
and imports the installed package ZIP before running
the coordinator, so published comparison code is unnecessary. It obtains its own
invocation and artifact identity from BuildBuddy. The parent UUID is read from the
runner-generated `buildbuddy.bazelrc` metadata when no explicit invocation ID is
provided. Cold comparisons submit one GPU child; fully cached comparisons submit
zero GPU children. CPU and GPU jobs share the dedicated executor; the host-wide
lock serializes inference across all participating processes.

Inside an existing BuildBuddy parent invocation, the equivalent direct coordinator
command from the implementation checkout is:

```sh
python3 -m tools.comparison.buildbuddy coordinate \
  --config /var/lib/llm-cc/comparison.json \
  --repository pawelchcki/llm-cc \
  --head "$(git rev-parse HEAD)" --branch "$(git branch --show-current)" \
  --pipeline-id "$(python3 -c 'from tools.comparison.submit_bazzite import parent_invocation_id; print(parent_invocation_id())')" \
  --output-dir "$BUILDBUDDY_ARTIFACTS_DIRECTORY"
```

Use a real parent invocation ID and its artifact directory. Coordinator PR discovery
uses the actual open PR head and target; branches without an open PR skip inference.
Default-branch runs populate the baseline. Generated comment publication remains
the separate ci-toolkit deployment step. Exercise the largest eligible files and a
warm comparison before enabling automatic triggers.
