# Release distribution

The release workflow builds five standalone executables, then
`tools/release_assets.py verify` checks every executable/backend checksum and
commit before packaging them. `SHA256SUMS` covers the package artifacts as
well as the standalone binaries. Packages never rebuild the native code.

| Channel | Release artifact | Installed command |
| --- | --- | --- |
| npm/npx | `llm-cc-X.Y.Z.tgz` and five `pawelchcki-llm-cc-*-X.Y.Z.tgz` platform packages | `llm-cc` |
| pip | Five `llm_cc-X.Y.Z-py3-none-PLATFORM.whl` wheels | `llm-cc` or `python -m llm_cc` |
| Homebrew (publication deferred) | `llm-cc.rb` and four `llm-cc-X.Y.Z-PLATFORM.tar.gz` archives | `llm-cc` |

The npm package pins optional platform dependencies to its own version. It
has no install scripts or runtime downloads. Python wheels use the release's
glibc/macOS compatibility floors and bundle an executable with its Unix mode
bits. Python replaces its process on Unix; the npm shim forwards stdio,
signals and the binary's exit status. Models and GPU backends retain the
native CLI's existing download/cache behavior.

Prepared Homebrew artifacts use a prebuilt formula for a future custom tap, following
[cargo-dist's Homebrew approach](https://axodotdev.github.io/cargo-dist/book/installers/homebrew.html).
The formula selects the OS and architecture and verifies the archive's
SHA-256 before installing its `bin/llm-cc`. A formula supports the CLI on
Linux and macOS, including normal `brew upgrade` behavior. Homebrew publication
and CI installation checks are deferred; npm and PyPI releases do not require
a tap or Homebrew credentials.

## Publishing setup

GitHub Actions publishes the verified release. Runnerless publishes npm and
PyPI packages after the successful release workflow completes. Dry-run
release builds do not publish registry packages.

Deploy the updated runnerless compiler/runtime and GitHub bridge from
`my-infra` before enabling this repository's new publishing capabilities.

The runnerless host needs publishing grants for `pawelchcki/llm-cc` and these
exact npm package names:

```text
llm-cc
@pawelchcki/llm-cc-linux-x64
@pawelchcki/llm-cc-linux-arm64
@pawelchcki/llm-cc-darwin-arm64
@pawelchcki/llm-cc-darwin-x64
@pawelchcki/llm-cc-win32-x64
```

It also needs a PyPI grant for `llm-cc`. Registry credentials belong to the
host; repository CI code supplies package names and release asset names.
The host validates the published release/tag commit, asset checksums and
archive metadata before publishing. Npm platform packages publish before
the wrapper. Repeated operations accept an existing package only when its
bytes match the verified release artifact.

The host grant configuration uses these bindings:

```json
{
  "pawelchcki/llm-cc": {
    "npm": {
      "packages": [
        "llm-cc",
        "@pawelchcki/llm-cc-linux-x64",
        "@pawelchcki/llm-cc-linux-arm64",
        "@pawelchcki/llm-cc-darwin-arm64",
        "@pawelchcki/llm-cc-darwin-x64",
        "@pawelchcki/llm-cc-win32-x64"
      ],
      "tokenBinding": "REGISTRY_LLM_CC_NPM_TOKEN"
    },
    "pypi": {
      "packages": ["llm-cc"],
      "tokenBinding": "REGISTRY_LLM_CC_PYPI_TOKEN"
    }
  }
}
```

Store the named credentials in a Runnerless application secret set, assign
the set to the repository's Production environment, and enable Production.
The application credentials take precedence over host Worker secrets. For
the host-secret fallback, `my-infra` reads the OS keyring entries
`runnerless-llm-cc-npm-token` and `runnerless-llm-cc-pypi-token` through
`bazel run //workers/ci-toolkit:registry_credentials`. Neither flow exposes
token values to the repository program.

Confirm that the publishing accounts control the npm names/scope and PyPI
project before the first release. The repository's `.runnerless-ci.ts`
declares the publish operations; host grants and credentials are configured
in `my-infra`.

## Verification

```sh
python3 tools/release_assets_test.py
python3 tools/distribution_packages_test.py
LLM_CC_TEST_HOMEBREW=1 python3 tools/distribution_packages_test.py
```

The small tests install generated packages through real pip, npm and npx
using temporary directories and a local npm registry. They check wheel
selection and ABI rejection, checksums, permissions, reproducibility,
argument/stream/environment/cwd forwarding, signals and exit codes.
The optional Homebrew test installs through a temporary tap and runs the
formula's test, then removes that installation and tap. It needs an unused
`llm-cc` Homebrew installation slot.

Release CI repeats pip/npm/npx installs against the actual executable on
all five target platforms. Homebrew artifacts retain contract tests and an
optional local installation check; Homebrew is outside the active CI and
publishing flow.
