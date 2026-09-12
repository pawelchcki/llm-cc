This is a complete consuming root for llm-cc. It uses the checkout two directories
above it; run `bazel build --config=release --config=cpu @llm_cc//:install` here.
Use Bazel 9.2.0, as pinned by the main checkout.

For a pinned remote source, replace `local_path_override` with:

```starlark
git_override(
    module_name = "llm_cc",
    remote = "https://github.com/pawelchcki/llm-cc.git",
    commit = "<full llm-cc commit>",
)
```

Copy the two files in `patches/` from that same llm-cc revision. Bazel requires
`single_version_override` patches to belong to the consuming root; dependency
module overrides are ignored. `tools/check_consumer.sh` checks these copies
against the source and exercises a renamed dependency in a separate Git repository.
Use `tools/check_consumer.sh --tests-only` to run installer, CLI, language, TLS,
and comparison regressions without building an installation. The harness cleans
up its own Bazel output directory; `CONSUMER_OUTPUT_BASE` optionally supplies a
reusable directory. Universal payload validation remains a separate GPU build.

Toolchain registration is explicit. The public `toolchains` extension creates
LLVM, the portable sysroot, and pinned CUDA repositories. `override_repo` routes
rules_cuda's internal toolkit reference to that same CUDA repository. The
foreign-build toolchain registrations supply CMake/Ninja and unused Make/M4/
pkg-config slots. No system CUDA toolkit, GCC compiler, or GNU Make is needed.

Provide llm-cc's own identity, independently of the consumer's workspace status:

```sh
bazel run --config=release --config=cuda \
  --@llm_cc//:source_version=0.2.0-dev.123+g0123456 \
  --@llm_cc//:source_commit=<full-llm-cc-commit> \
  @llm_cc//:install -- --prefix /opt/llm-cc
```

`--@llm_cc//:artifact_base_url=URL` optionally supplies an artifact resolver.
Without explicit provenance, external builds use the source `version.txt`
unknown-build fallback; they never stamp the consumer's Git HEAD. Source builds
keep downloads disabled by default. The installed executable can run CPU
inference with a local model and `--no-download`.

For an A10-only deployment, add `--config=a10`, or set
`--@llm_cc//:cuda_archs=compute_86:sm_86`. Specifications are separated by semicolons,
with comma-separated output architectures after a colon (rules_cuda syntax).
The release default keeps all six specifications. Empty and malformed selections
fail analysis. Kernel selection and cuBLAS archive coverage remain unchanged.

Backend cache/install keys include source provenance and a configuration SHA-256
covering pinned sources, SDK/build recipes, target CPU, build mode and normalized
CUDA architectures. Both binary and bundle consume one provenance provider.
Manifests must match the version, commit, backend name and configuration before a
bundle is loaded or fetched. Previous cache entries are left in place; public
artifact filenames are unchanged. A custom architecture build needs bundles built
with the same setting.
