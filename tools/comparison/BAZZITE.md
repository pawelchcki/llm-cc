# Bazzite host setup

The two Bazzite BuildBuddy executors share one Radeon RX 7900 XTX. The `ccd1`
executor uses the dedicated `linux-amd64-rocm` pool for bare-host dogfooding;
`ccd0` retains the general `linux-amd64-kvm` pool. Use the ROCm profile and one
worker. Both the CPU coordinator and GPU child target the dedicated pool, where
the executor's 12 logical CPUs and 60 GiB memory allow both jobs to run together
at their requested one compute unit each. The worker also holds a host-wide file
lock so independent invocations cannot score concurrently. The lock covers this
comparison pipeline; unrelated GPU applications must follow the same convention
to participate.

Install the pinned baseline scorer and ROCm backend in
`/var/lib/llm-cc/scorers/4646123-rocm`, retain the existing checksum-pinned model,
and supply an existing shared filesystem cache directory. Generate the profile
with `python3 -m tools.comparison.profile --backend rocm --execution-image none`
and the `--execution-host` JSON identity. That identity must include the physical
GPU PCI address, architecture, minimum VRAM, shared resource ID
`bazzite-radeon-0`, and hashes of the actual host ROCm runtime libraries.
The profile records all inference settings and installed scorer checksums.

Publish a reproducible package and configuration from the implementation checkout:

```sh
sudo python3 -m tools.comparison.setup_bazzite \
  --installed-root /var/lib/llm-cc/scorers/4646123-rocm \
  --model /var/home/pawel/.cache/llm-cc/models/DeepSeek-Coder-V2-Lite-Base-Q6_K.gguf \
  --profile /path/to/verified-rocm-profile.json \
  --cache /var/lib/llm-cc/cache \
  --execution-commit 4646123b274005c587dfeb614f17ddf5fd36aef6 \
  --lock-group pawel
```

Use the actual executor user's group for `--lock-group`. The helper checks every
installed and runtime file digest and the complete model checksum, then creates:

- `packages/<sha256>.zip`: a deterministic snapshot containing only the comparison
  implementation and classification rules. It includes working-tree implementation
  changes, so remote jobs do not require an uncommitted package to exist on GitHub.
- `configurations/<identity>/`: profile, rules, and nonsecret comparison settings.
- `comparison.json`: atomically published current settings.
- `locks/bazzite-radeon-0.lock`: a root-owned group-writable persistent lock beneath
  root-owned directories. Keep this inode in place while workers may be running.

Configuration points at existing host scorer/model/cache paths. The helper never
downloads model weights, provisions an external store, or copies credentials. The
cache and model must be accessible to both executor users. Package and configuration
files are readable by both workers; their parent directories should remain
root-owned. Re-run setup whenever comparison source or the scoring profile changes.
The source ZIP hash pins executed comparison code; the Git execution commit is
repository metadata. Scoring cache identity remains the verified profile.

Workers use the ZIP bootstrap's checksum verification before loading any bundled
Python code. Device identity and runtime checksums must validate before inference;
label routing remains a best-effort scheduling preference. The dedicated pool
keeps host-local packages, model, and cache accessible to both stages. The generic
pool also contains other machines, so GPU labels alone cannot make it suitable.
A wrong host fails with an explicit worker artifact. Only actual cache misses
create GPU worker requests.

To submit a manual **CPU coordinator** from the implementation checkout, export
`BUILDBUDDY_API_KEY` and optionally `GITHUB_TOKEN` through your existing credential
mechanism, then run:

```sh
python3 -m tools.comparison.submit_bazzite submit \
  --config /var/lib/llm-cc/comparison.json \
  --repository pawelchcki/llm-cc \
  --head "$(git rev-parse HEAD)" --branch "$(git branch --show-current)"
```

The command prints the new parent invocation URL. It passes credentials through
BuildBuddy's sensitive remote headers and checks out the requested application
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
