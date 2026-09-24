#!/bin/sh
# Install a prebuilt llm-cc release on Linux or macOS with curl or wget.
# Run "sh install.sh --help" or see usage() below.
set -eu

usage() {
  cat <<'EOF'
Usage: sh -c "$(curl -fsSL https://github.com/pawelchcki/llm-cc/releases/latest/download/install.sh)" -- [options]
       sh -c "$(wget -qO- https://github.com/pawelchcki/llm-cc/releases/latest/download/install.sh)" -- [options]

Downloads the llm-cc release for this platform, verifies SHA-256, and installs
the executable. Model weights are never installed; llm-cc downloads them on
first use.

Options:
  --version X.Y.Z   Install a specific release (default: latest)
  --bin-dir PATH    Executable directory (default: $HOME/.local/bin)
  --backend NAME    Linux x86-64 GPU bundle: cuda, rocm, auto, or none
                    (default: auto, which fetches only for a detected GPU)
  --no-modify-path  Never print PATH guidance
  -h, --help        Show this help

Environment: LLM_CC_VERSION, LLM_CC_BIN_DIR, LLM_CC_BACKEND, and
LLM_CC_RELEASE_BASE (an alternative release directory, such as a mirror; the
executable and any backend bundle are both fetched from it).
EOF
}

repository="pawelchcki/llm-cc"
version="${LLM_CC_VERSION:-latest}"
bin_dir="${LLM_CC_BIN_DIR:-${HOME:?HOME is unset}/.local/bin}"
backend="${LLM_CC_BACKEND:-auto}"
modify_path=1

fail() {
  printf 'llm-cc install: error: %s\n' "$1" >&2
  exit 1
}

while [ $# -gt 0 ]; do
  case "$1" in
    --version) [ $# -ge 2 ] || fail "--version requires a value"; version="$2"; shift 2 ;;
    --version=*) version="${1#--version=}"; shift ;;
    --bin-dir) [ $# -ge 2 ] || fail "--bin-dir requires a path"; bin_dir="$2"; shift 2 ;;
    --bin-dir=*) bin_dir="${1#--bin-dir=}"; shift ;;
    --backend) [ $# -ge 2 ] || fail "--backend requires a name"; backend="$2"; shift 2 ;;
    --backend=*) backend="${1#--backend=}"; shift ;;
    --no-modify-path) modify_path=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) fail "unknown argument: $1" ;;
  esac
done

case "$backend" in
  cuda|rocm|auto|none) ;;
  *) fail "--backend must be cuda, rocm, auto, or none" ;;
esac

# Downloader: curl preferred, wget accepted. Both follow GitHub redirects.
if command -v curl >/dev/null 2>&1; then
  download() { curl -fsSL --retry 3 --connect-timeout 15 -o "$2" "$1"; }
elif command -v wget >/dev/null 2>&1; then
  download() { wget -q --tries=3 --timeout=60 -O "$2" "$1"; }
else
  fail "curl or wget is required"
fi

if command -v sha256sum >/dev/null 2>&1; then
  sha256_of() { sha256sum "$1" | cut -d' ' -f1; }
elif command -v shasum >/dev/null 2>&1; then
  sha256_of() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
  fail "sha256sum or shasum is required to verify downloads"
fi

system="$(uname -s)"
machine="$(uname -m)"
case "$system" in
  Linux) os=linux ;;
  Darwin) os=macos ;;
  *) fail "unsupported system '$system'; Windows users: py -3 install_release.py from the release" ;;
esac
case "$machine" in
  x86_64|amd64) arch=x86_64 ;;
  aarch64|arm64) arch=arm64 ;;
  *) fail "unsupported architecture '$machine'" ;;
esac
platform="$os-$arch"

# Rosetta reports x86_64 for an arm64 Mac running a translated shell.
if [ "$platform" = macos-x86_64 ] && [ "$(sysctl -n sysctl.proc_translated 2>/dev/null || echo 0)" = 1 ]; then
  platform=macos-arm64
fi

work="$(mktemp -d "${TMPDIR:-/tmp}/llm-cc-install.XXXXXX")"
cleanup() { rm -rf "$work"; }
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Resolve the version from the release manifest so "latest" never needs the
# GitHub API (which is rate limited for anonymous callers).
[ "$version" = latest ] || version="${version#v}"
release_base_override=""
if [ -n "${LLM_CC_RELEASE_BASE:-}" ]; then
  base="${LLM_CC_RELEASE_BASE%/}"
  release_base_override="$base"
elif [ "$version" = latest ]; then
  base="https://github.com/$repository/releases/latest/download"
else
  base="https://github.com/$repository/releases/download/v$version"
fi
download "$base/release-manifest.json" "$work/release-manifest.json" ||
  fail "cannot download $base/release-manifest.json"
manifest_version="$(sed -n 's/^[[:space:]]*"version":[[:space:]]*"\([0-9][0-9.]*\)".*/\1/p' "$work/release-manifest.json" | head -n 1)"
[ -n "$manifest_version" ] || fail "release manifest has no version"
if [ "$version" != latest ] && [ "$version" != "$manifest_version" ]; then
  fail "release manifest reports $manifest_version, expected $version"
fi
version="$manifest_version"
case "$version" in
  *[!0-9.]*|.*|*.) fail "invalid release version '$version'" ;;
esac

asset="llm-cc-$version-$platform"
printf 'Downloading llm-cc %s for %s\n' "$version" "$platform"
download "$base/$asset.sha256" "$work/$asset.sha256" || fail "no release for $platform at $base"
expected="$(cut -d' ' -f1 "$work/$asset.sha256")"
case "$expected" in
  *[!0-9a-f]*|'') fail "invalid checksum file for $asset" ;;
esac
[ "${#expected}" -eq 64 ] || fail "invalid checksum length for $asset"
download "$base/$asset" "$work/$asset" || fail "cannot download $base/$asset"
actual="$(sha256_of "$work/$asset")"
[ "$actual" = "$expected" ] || fail "SHA-256 mismatch for $asset (expected $expected, got $actual)"

mkdir -p "$bin_dir"
# Stage on the destination filesystem: the temporary directory may be mounted
# noexec, and the final rename must be atomic.
staged="$(mktemp "$bin_dir/.llm-cc.XXXXXX")"
remove_staged() { rm -f "$staged"; }
trap 'remove_staged; cleanup' EXIT
cp "$work/$asset" "$staged"
chmod 0755 "$staged"
"$staged" --version >/dev/null || fail "downloaded executable does not run on this host"
mv -f "$staged" "$bin_dir/llm-cc"
installed="$bin_dir/llm-cc"
printf 'Installed %s to %s\n' "$("$installed" --version)" "$installed"

# GPU backends are separate release components fetched by the executable from
# the same release, verified against its embedded commit.
if [ "$platform" = linux-x86_64 ] && [ "$backend" != none ]; then
  if [ "$backend" = auto ]; then
    backend=none
    if [ -e /dev/nvidiactl ] || command -v nvidia-smi >/dev/null 2>&1; then
      backend=cuda
    elif [ -e /dev/kfd ]; then
      backend=rocm
    fi
    [ "$backend" = none ] && printf 'No NVIDIA or AMD GPU detected; CPU execution is ready. Re-run with --backend cuda|rocm to add one.\n'
  fi
  if [ "$backend" != none ]; then
    printf 'Fetching the %s backend bundle\n' "$backend"
    # A release base override serves the bundle and its sidecars as well; the
    # executable otherwise fetches from the release recorded at build time.
    if [ -n "$release_base_override" ]; then
      set -- --url "$release_base_override/llm-cc-backend-$backend-linux-x86_64.bundle"
    else
      set --
    fi
    "$installed" backends fetch "$backend" "$@" --assume-yes ||
      fail "backend fetch failed; retry later with: llm-cc backends fetch $backend --assume-yes"
  fi
elif [ "$backend" != auto ] && [ "$backend" != none ]; then
  printf 'Note: %s bundles exist only for linux-x86_64; %s uses %s.\n' "$backend" "$platform" \
    "$([ "$os" = macos ] && echo Metal || echo CPU)"
fi

if [ "$modify_path" -eq 1 ]; then
  case ":${PATH:-}:" in
    *":$bin_dir:"*) ;;
    *) printf '\nAdd llm-cc to your PATH, for example in your shell startup file:\n  export PATH="%s:$PATH"\n' "$bin_dir" ;;
  esac
fi
