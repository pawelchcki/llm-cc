#!/usr/bin/env bash
# Every applied backend patch must be a provenance input.
set -euo pipefail

cd "$TEST_SRCDIR/$TEST_WORKSPACE"

python3 - "$@" <<'PY'
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

generator, listing, module_file, extensions_file, version_file = sys.argv[1:6]
configuration = [
    line for line in Path(listing).read_text().split() if line
]
# Patches that cannot change a produced backend binary.
ALLOWED_OUTSIDE = {
    # Build-log cosmetics for rules_foreign_cc.
    "third_party/rules_foreign_cc_reproducible_logs.patch",
    # The downloader's TLS stack, not the inference backends.
    "third_party/curl_boringssl_runtime.patch",
}
PATCHES = re.compile(r"patches\s*=\s*\[(.*?)\]", re.DOTALL)
LABELS = re.compile(r'"(//[^"]+)"')


def applied_patches(path):
    text = Path(path).read_text()
    found = set()
    for block in PATCHES.findall(text):
        for label in LABELS.findall(block):
            package, _, name = label[2:].partition(":")
            found.add(package + "/" + name if package else name)
    return found


failures = []
applied = applied_patches(module_file) | applied_patches(extensions_file)
if not applied:
    failures.append("no patches were found in MODULE.bazel or extensions.bzl")
for patch in sorted(applied):
    if patch in ALLOWED_OUTSIDE:
        if patch in configuration:
            failures.append(patch + " is allowlisted but listed as a provenance input")
    elif patch not in configuration:
        failures.append(patch + " is applied but is not a provenance input")
for patch in configuration:
    if not Path(patch).is_file():
        failures.append(patch + " is a provenance input but does not exist")
if failures:
    print("\n".join(failures), file=sys.stderr)
    sys.exit(1)

work = Path(os.environ.get("TEST_TMPDIR", "."))
generator = str(Path(generator).resolve())


def fingerprint(root, name):
    output = work / name
    output.mkdir(parents=True, exist_ok=True)
    header = output / "version.h"
    metadata = output / "provenance.env"
    subprocess.run(
        [
            generator,
            str(header),
            str(metadata),
            str(Path(version_file).resolve()),
            "",
            "",
            "",
            "compute_86:sm_86||",
            "",
            *[str(root / item) for item in configuration],
        ],
        check=True,
        cwd=str(work),
    )
    values = dict(
        re.findall(r"^(\w+)='(.*)'$", metadata.read_text(), re.MULTILINE)
    )
    digest = values["configuration"]
    if ('#define LLM_CC_BACKEND_CONFIGURATION "%s"' % digest) not in header.read_text():
        raise SystemExit("version.h and provenance.env disagree on the fingerprint")
    return digest


source = Path(".").resolve()
baseline = fingerprint(source, "baseline")
if fingerprint(source, "repeat") != baseline:
    raise SystemExit("provenance generation is not reproducible")

for index, changed in enumerate(configuration):
    mutated = work / ("mutated-%d" % index)
    if mutated.exists():
        shutil.rmtree(mutated)
    for item in configuration:
        target = mutated / item
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / item, target)
    with (mutated / changed).open("ab") as stream:
        stream.write(b"\n")
    if fingerprint(mutated, "changed-%d" % index) == baseline:
        raise SystemExit(changed + " does not affect the backend configuration")

print("provenance covers %d configuration inputs" % len(configuration))
PY
