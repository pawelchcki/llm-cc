"""Check ZIP artifact transfer using deterministic scorer/coordinator images.

Run from the repository root with PYTHONPATH=. and supply the test images by
digest. Each extraction, preparation, worker and aggregation starts a fresh
rootless Podman container as UID 10001. No model inference or PR publication
is tested. See README.md for building the images.
"""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time
import zipfile

from tools.comparison.fixtures import commit_files, git
from tools.comparison.common import read_json, write_json
from tools.comparison.ci import image_reference


def run(args):
    image_reference(args.coordinator, "coordinator")
    image_reference(args.scorer, "scorer")
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    archives = root / "archives"
    archives.mkdir()
    cache = root / "cache"
    cache.mkdir()
    repo = root / "repository"
    repo.mkdir()
    git(repo, "init", "-q", "-b", "main")
    base = commit_files(repo, {"src/base.c": "int base;\n"}, "baseline")
    head = commit_files(
        repo,
        {
            "src/one.c": "int one;\n",
            "src/two.c": "int two;\n",
            "include/api.h": "int api(void);\n",
        },
        "four unique contents",
    )
    sequence = 0

    def container(job, image, *command):
        nonlocal sequence
        sequence += 1
        with (root / ("container-%02d.log" % sequence)).open("wb") as log:
            subprocess.run(
                [
                    "podman",
                    "run",
                    "--rm",
                    "--network=none",
                    "--userns=keep-id:uid=10001,gid=10001",
                    "--user=10001:10001",
                    "--volume",
                    str(job) + ":/work:z",
                    "--volume",
                    str(archives) + ":/artifacts:ro,z",
                    "--volume",
                    str(cache) + ":/cache:z",
                    "--workdir=/work",
                    "--env=LLM_CC_CACHE_DIR=/work/native-cache",
                    "--env=LLM_CC_ENTROPY_CACHE_DIR=/work/entropy-cache",
                    image,
                    "sh",
                    "-ec",
                    'test "$(id -u)" = 10001; id; exec "$@"',
                    "acceptance",
                    *command,
                ],
                stdout=log,
                stderr=subprocess.STDOUT,
                check=True,
                timeout=120,
            )

    def archive(job, path, name):
        with zipfile.ZipFile(archives / name, "w", zipfile.ZIP_DEFLATED) as bundle:
            for file in sorted((job / path).rglob("*")):
                if file.is_file():
                    bundle.write(file, file.relative_to(job))

    def extract(job, names):
        # The coordinator has Python; the scorer image does not. Artifact
        # extraction is a separate non-root container before each native job.
        container(
            job,
            args.coordinator,
            "python3",
            "-c",
            "import sys,zipfile; "
            "[zipfile.ZipFile('/artifacts/'+name).extractall('/work') "
            "for name in sys.argv[1:]]",
            *names,
        )

    def prepare(name):
        job = root / name
        job.mkdir()
        shutil.copytree(repo, job / "repository")
        write_json(
            job / "comparison/identity.json",
            {
                "repository": "acceptance/fixture",
                "pipeline_id": name,
            },
        )
        container(
            job,
            args.coordinator,
            "llm-cc",
            "compare",
            "prepare",
            "--repo",
            "/work/repository",
            "--head",
            head,
            "--target",
            base,
            "--identity",
            "/work/comparison/identity.json",
            "--cache",
            "/cache",
            "--max-workers",
            "4",
            "--scorer-image",
            args.scorer,
            "--output-dir",
            "/work/comparison/prep",
            "@/opt/llm-cc-comparison/scoring.args",
        )
        return job, read_json(job / "comparison/prep/plan.json")

    prepared, plan = prepare("cold")
    if plan["scorer"]["inference_abi"] != "deterministic-test/entropy-v1":
        raise RuntimeError("this experiment requires the deterministic test scorer")
    if len(plan["workers"]) != 4:
        raise RuntimeError("fixture did not produce four workers")
    archive(prepared, "comparison", "prepare.zip")
    worker_archives = []
    expected_bytes = {}
    for worker in plan["workers"]:
        worker_id = worker["worker_id"]
        job = root / ("worker-%d" % worker_id)
        job.mkdir()
        extract(job, ["prepare.zip"])
        output = "comparison/workers/%d" % worker_id
        container(
            job,
            args.scorer,
            "/opt/llm-cc/bin/llm-cc",
            "compare",
            "worker",
            "--plan",
            "/work/comparison/prep/plan.json",
            "--worker-id",
            str(worker_id),
            "--cache",
            "/cache",
            "--model",
            "/models/model.gguf",
            "--scorer-image",
            args.scorer,
            "--output-dir",
            "/work/" + output,
        )
        artifact = output + "/worker-%d.json" % worker_id
        if read_json(job / artifact)["status"] != "complete":
            raise RuntimeError("worker failed: " + artifact)
        expected_bytes[artifact] = (job / artifact).read_bytes()
        name = "worker-%d.zip" % worker_id
        archive(job, output, name)
        worker_archives.append(name)

    aggregate = root / "aggregate"
    aggregate.mkdir()
    # Worst download order: every completed worker, followed by preparation.
    extract(aggregate, worker_archives + ["prepare.zip"])
    for path, contents in expected_bytes.items():
        if (aggregate / path).read_bytes() != contents:
            raise RuntimeError("preparation overwrote " + path)
    workers = [
        value for path in expected_bytes for value in ("--worker", "/work/" + path)
    ]
    container(
        aggregate,
        args.coordinator,
        "llm-cc",
        "compare",
        "aggregate",
        "--plan",
        "/work/comparison/prep/plan.json",
        *workers,
        "--output-dir",
        "/work/comparison/report",
    )
    cold_report = read_json(aggregate / "comparison/report/report.json")
    started = time.monotonic()
    warm, warm_plan = prepare("warm")
    if warm_plan["workers"] or warm_plan["cache_stats"]["misses"]:
        raise RuntimeError("warm preparation scheduled workers")
    container(
        warm,
        args.coordinator,
        "llm-cc",
        "compare",
        "aggregate",
        "--plan",
        "/work/comparison/prep/plan.json",
        "--output-dir",
        "/work/comparison/report",
    )
    elapsed = time.monotonic() - started
    warm_report = read_json(warm / "comparison/report/report.json")
    if cold_report["status"] != "complete" or warm_report["status"] != "complete":
        raise RuntimeError("incomplete report")
    if cold_report["comparisons"] != warm_report["comparisons"] or elapsed >= 60:
        raise RuntimeError("warm comparison changed totals or exceeded 60 seconds")
    evidence = {
        "inference": "deterministic test scorer; no GPU inference",
        "coordinator_image": args.coordinator,
        "scorer_image": args.scorer,
        "scorer": plan["scorer"],
        "model": plan["model"],
        "container_uid": 10001,
        "fresh_containers": sequence,
        "cold_workers": 4,
        "warm_workers": 0,
        "warm_seconds": elapsed,
        "reports_complete": True,
        "identical_cold_warm_totals": True,
        "worker_first_zip_extraction_preserved_results": True,
        "worker_artifact_sha256": {
            path: hashlib.sha256(value).hexdigest()
            for path, value in expected_bytes.items()
        },
    }
    write_json(root / "acceptance.json", evidence)
    print(json.dumps(evidence, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--coordinator", required=True)
    parser.add_argument("--scorer", required=True)
    parser.add_argument("--output", required=True, type=Path)
    run(parser.parse_args())
