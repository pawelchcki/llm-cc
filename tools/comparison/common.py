import hashlib
import json
import os
import subprocess
import tempfile
from pathlib import Path


def canonical_bytes(value):
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=True, allow_nan=False
    ).encode("utf-8")


def digest(value):
    return hashlib.sha256(canonical_bytes(value)).hexdigest()


def pipeline_prefix(identity):
    """The store prefix for one pipeline's transport objects and reports."""
    # Pipeline IDs are opaque input, never filesystem paths.
    return (
        "pipelines/" + digest([identity["repository"], identity["pipeline_id"]]) + "/"
    )


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp.%d" % os.getpid())
    temporary.write_bytes(canonical_bytes(value) + b"\n")
    os.replace(temporary, path)


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def llm_cc():
    """The llm-cc executable that renders reports: $LLM_CC, else PATH."""
    return os.environ.get("LLM_CC") or "llm-cc"


def aggregate_report(
    output_dir,
    plan=None,
    workers=(),
    identity=None,
    errors=(),
    executable=None,
    fingerprint=None,
):
    """Write a report set with `llm-cc compare aggregate`; returns report.json.

    Without a plan, `errors` make a failure report for `identity`. With both,
    the plan is still aggregated into analysis-report.json before the errors
    turn the published report into a failure.
    """
    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    command = [
        executable or llm_cc(),
        "compare",
        "aggregate",
        "--output-dir",
        str(output),
    ]
    if plan is not None:
        command += ["--plan", str(plan)]
    elif fingerprint is not None:
        command += ["--fingerprint", fingerprint]
    for worker in workers:
        command += ["--worker", str(worker)]
    for error in errors:
        command += ["--error", str(error)]
    with tempfile.TemporaryDirectory(prefix="llm-cc-identity-") as temporary:
        if identity is not None:
            identity_path = Path(temporary) / "identity.json"
            write_json(identity_path, identity)
            command += ["--identity", str(identity_path)]
        try:
            completed = subprocess.run(
                command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False
            )
        except OSError as error:
            raise RuntimeError(
                "cannot run %s compare aggregate: %s" % (command[0], error)
            ) from error
    if completed.returncode not in (0, 1):
        raise RuntimeError(
            "llm-cc compare aggregate failed (exit %d): %s"
            % (
                completed.returncode,
                completed.stderr.decode("utf-8", "replace").strip(),
            )
        )
    return read_json(output / "report.json")
