"""GPU worker stage for a prepared comparison plan."""

from __future__ import annotations

from contextlib import contextmanager
import hashlib
import json
import math
import os
import re
import stat
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Any, Iterable

from .cache import ResultCache
from .deadline import Deadline, DeadlineExceeded


class WorkerError(RuntimeError):
    pass


def _json(value: Any) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), allow_nan=False
    ).encode()


def _hash(path: Path, deadline: float | None = None) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        before = os.fstat(source.fileno())
        for block in iter(lambda: source.read(1024 * 1024), b""):
            if deadline is not None and time.monotonic() > deadline:
                raise WorkerError("worker deadline exceeded while hashing")
            digest.update(block)
        after = os.fstat(source.fileno())
    if (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) != (
        after.st_dev,
        after.st_ino,
        after.st_size,
        after.st_mtime_ns,
    ):
        raise WorkerError("file changed while being hashed: %s" % path)
    return digest.hexdigest()


def _host_gpu_environment(host, deadline, sysfs=Path("/sys"), devices=Path("/dev")):
    """Fail closed on actual GPU/runtime identity, independently of runner labels."""
    if (
        not isinstance(host, dict)
        or host.get("gpu_vendor") != "amd"
        or not re.fullmatch(
            r"[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]",
            host.get("gpu_pci_address", ""),
        )
        or not re.fullmatch(r"gfx[0-9]+", host.get("gpu_arch", ""))
        or type(host.get("gpu_vram_bytes_min")) is not int
        or host["gpu_vram_bytes_min"] <= 0
        or not re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,63}", host.get("resource_id", ""))
        or not isinstance(host.get("runtime_files"), dict)
        or not host["runtime_files"]
    ):
        raise WorkerError("bare-host GPU/runtime identity is incomplete")
    for filename, checksum in host["runtime_files"].items():
        path = Path(filename)
        if (
            not path.is_absolute()
            or not re.fullmatch(r"[0-9a-f]{64}", checksum)
            or not path.is_file()
            or _hash(path, deadline) != checksum
        ):
            raise WorkerError("bare-host runtime checksum mismatch: " + filename)
    pci = host["gpu_pci_address"]
    device = sysfs / "bus/pci/devices" / pci
    if int((device / "vendor").read_text(), 0) != 0x1002:
        raise WorkerError("selected PCI device is not an AMD GPU")
    if int((device / "mem_info_vram_total").read_text()) < host["gpu_vram_bytes_min"]:
        raise WorkerError("selected GPU has insufficient physical VRAM")
    domain, bus, slot = pci.split(":")
    number, function = slot.split(".")
    location = (int(bus, 16) << 8) | (int(number, 16) << 3) | int(function)
    matches = []
    for node in (sysfs / "class/kfd/kfd/topology/nodes").glob("*"):
        properties = dict(
            line.split() for line in (node / "properties").read_text().splitlines()
        )
        if (
            int(properties.get("vendor_id", "0")) == 0x1002
            and int(properties.get("domain", "-1")) == int(domain, 16)
            and int(properties.get("location_id", "-1")) == location
        ):
            matches.append(properties)
    if len(matches) != 1:
        raise WorkerError("selected GPU must match exactly one accessible KFD node")
    gpu = matches[0]
    target = int(gpu["gfx_target_version"])
    arch = "gfx%d%d%d" % (target // 10000, target // 100 % 100, target % 100)
    if arch != host["gpu_arch"]:
        raise WorkerError("selected GPU architecture differs from profile")
    unique_id = int(gpu.get("unique_id", "0"))
    if not 0 < unique_id < 2**64:
        raise WorkerError("selected GPU has no usable ROCm UUID")
    for path in (
        devices / "kfd",
        devices / "dri" / ("renderD" + gpu["drm_render_minor"]),
    ):
        if not stat.S_ISCHR(path.stat().st_mode) or not os.access(
            path, os.R_OK | os.W_OK
        ):
            raise WorkerError("GPU device is not readable and writable: " + str(path))
    # ROCr UUIDs encode the KFD unique_id as 16 hexadecimal digits. Restrict at
    # ROCr level first; HIP sees precisely one device, ordinal zero.
    return {
        "ROCR_VISIBLE_DEVICES": "GPU-%016x" % unique_id,
        "HIP_VISIBLE_DEVICES": "0",
        "CUDA_VISIBLE_DEVICES": "0",
    }


def _validate_lock_directory(lock_root):
    if lock_root.resolve() != lock_root:
        raise WorkerError("GPU lock directory must not traverse symlinks")
    for directory in [lock_root, *lock_root.parents]:
        metadata = directory.stat()
        if metadata.st_uid != 0 or metadata.st_mode & 0o022:
            raise WorkerError(
                "GPU lock directory and ancestors must be root-owned and not group/world writable"
            )


@contextmanager
def _gpu_lease(resource_id, deadline, lock_root=Path("/var/lib/llm-cc/locks")):
    """One shared, preprovisioned inode per physical GPU across both executors.

    Never create or unlink lock files: replacing an inode defeats flock. The
    scorer inherits the descriptor so abrupt worker death cannot free its GPU.
    """
    import fcntl

    if not re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,63}", resource_id):
        raise WorkerError("invalid shared GPU resource ID")
    path = lock_root / (resource_id + ".lock")
    _validate_lock_directory(lock_root)
    try:
        fd = os.open(path, os.O_RDWR | os.O_NOFOLLOW)
    except OSError as error:
        raise WorkerError("preprovision shared GPU lock " + str(path)) from error
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            raise WorkerError("GPU lock must be a regular file")
        while True:
            if time.monotonic() >= deadline:
                raise WorkerError("worker deadline exceeded waiting for shared GPU")
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                time.sleep(min(0.2, max(0, deadline - time.monotonic())))
        yield fd
    finally:
        os.close(fd)


def _write_artifact(output_dir: Path, artifact: dict[str, Any]) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    target = output_dir / ("worker-%s.json" % artifact["worker_id"])
    temporary = target.with_suffix(".tmp")
    temporary.write_bytes(_json(artifact))
    os.replace(temporary, target)


def _expected_configuration(scoring: dict[str, Any]) -> dict[str, Any]:
    """Profile configuration fields that must be echoed by llm-cc.

    `argv` and a few orchestration fields are intentionally not scorer fields.
    Profiles can also provide a nested expected_configuration to make a strict
    contract explicit as the CLI evolves.
    """
    if isinstance(scoring.get("expected_configuration"), dict):
        return scoring["expected_configuration"]
    excluded = {"argv", "command", "expected_configuration", "language", "model"}
    return {key: value for key, value in scoring.items() if key not in excluded}


def _validate_stream(
    stdout: bytes,
    expected: list[dict[str, Any]],
    fingerprint: str,
    scoring: dict[str, Any],
    model_sha256: str | None,
    model_bytes: int | None,
) -> list[dict[str, Any]]:
    try:
        text = stdout.decode("utf-8")
    except UnicodeDecodeError as error:
        raise WorkerError("scorer stdout is not UTF-8") from error
    if not text.endswith("\n"):
        raise WorkerError("truncated scorer JSONL stream")

    def reject_constant(value):
        raise WorkerError("non-finite JSON number: " + value)

    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise WorkerError("duplicate JSON field: " + key)
            result[key] = value
        return result

    events = []
    for number, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            raise WorkerError("blank JSONL line %d" % number)
        try:
            event = json.loads(
                line, parse_constant=reject_constant, object_pairs_hook=unique_object
            )
        except json.JSONDecodeError as error:
            raise WorkerError("invalid JSONL line %d" % number) from error
        if not isinstance(event, dict):
            raise WorkerError("JSONL line %d is not an object" % number)
        if event.get("type") not in {
            "start",
            "configuration",
            "warning",
            "file_start",
            "file",
            "totals",
        }:
            raise WorkerError("unexpected scorer event on line %d" % number)
        events.append(event)
    configs = [event for event in events if event.get("type") == "configuration"]
    totals = [event for event in events if event.get("type") == "totals"]
    if len(configs) != 1:
        raise WorkerError("scorer must emit exactly one configuration event")
    if len(totals) != 1 or events[-1] is not totals[0]:
        raise WorkerError("scorer must end with one totals event")
    config = configs[0]
    for key, value in _expected_configuration(scoring).items():
        if (
            key not in config
            or config[key] != value
            or (isinstance(value, bool) and type(config[key]) is not bool)
        ):
            raise WorkerError("scorer configuration mismatch for %s" % key)
    if model_sha256 and config.get("model_sha256") != model_sha256:
        raise WorkerError("scorer model digest differs from profile")
    if model_bytes is not None and config.get("model_size") != model_bytes:
        raise WorkerError("scorer model size differs from profile")
    required = {
        "language": expected[0]["language"],
        "no_download": True,
        "no_ignore": True,
        "include_headers": True,
    }
    for key, value in required.items():
        if config.get(key) != value or (
            isinstance(value, bool) and type(config.get(key)) is not bool
        ):
            raise WorkerError("scorer did not apply required %s" % key)
    file_events = [event for event in events if event.get("type") == "file"]
    if file_events and events.index(config) > events.index(file_events[0]):
        raise WorkerError("scorer configuration must precede file results")
    if len(file_events) != len(expected):
        raise WorkerError("scorer did not return every expected file")
    by_path: dict[str, dict[str, Any]] = {}
    for event in file_events:
        path = event.get("path")
        if not isinstance(path, str) or path in by_path:
            raise WorkerError("duplicate or invalid scorer file path")
        by_path[path] = event
    results = []
    # Paths emitted by the scorer may be absolute or relative to the worker cwd.
    expected_paths = {str(item["_input"]): item for item in expected}
    expected_paths.update(
        {
            str(item["_original_input"]): item
            for item in expected
            if "_original_input" in item
        }
    )
    used_keys: set[str] = set()
    for path, event in by_path.items():
        item = expected_paths.get(path) or expected_paths.get(str(Path(path).resolve()))
        if item is None:
            raise WorkerError("scorer returned unexpected file %r" % path)
        if item["key"] in used_keys:
            raise WorkerError("scorer returned an input more than once")
        used_keys.add(item["key"])
        if event.get("language") != item["language"]:
            raise WorkerError("language mismatch for %r" % path)
        llm_cc, tokens = event.get("llm_cc"), event.get("token_count")
        if (
            not isinstance(llm_cc, (int, float))
            or isinstance(llm_cc, bool)
            or not math.isfinite(llm_cc)
            or llm_cc < 0
        ):
            raise WorkerError("invalid llm_cc for %r" % path)
        if not isinstance(tokens, int) or isinstance(tokens, bool) or tokens < 0:
            raise WorkerError("invalid token_count for %r" % path)
        results.append(
            {
                "schema_version": 1,
                "key": item["key"],
                "fingerprint": fingerprint,
                "content_sha256": item["content_sha256"],
                "language": item["language"],
                "llm_cc": llm_cc,
                "token_count": tokens,
            }
        )
    totals_event = totals[0]
    for field in ("failed", "analyzed", "discovered", "token_count"):
        if type(totals_event.get(field)) is not int or totals_event[field] < 0:
            raise WorkerError("invalid totals count: " + field)
    if (
        "failed" not in totals_event
        or "partial" not in totals_event
        or totals_event["failed"] != 0
        or totals_event["partial"] is not False
    ):
        raise WorkerError("scorer reported incomplete totals")
    if totals_event.get("analyzed") != len(expected) or totals_event.get(
        "discovered"
    ) != len(expected):
        raise WorkerError("scorer totals do not cover expected files")
    if totals_event.get("token_count") != sum(
        value["token_count"] for value in results
    ):
        raise WorkerError("scorer totals token count mismatch")
    total_score = sum(float(value["llm_cc"]) for value in results)
    if (
        isinstance(totals_event.get("llm_cc"), bool)
        or not isinstance(totals_event.get("llm_cc"), (int, float))
        or not math.isfinite(totals_event["llm_cc"])
        or not math.isclose(
            float(totals_event["llm_cc"]), total_score, rel_tol=1e-10, abs_tol=1e-10
        )
    ):
        raise WorkerError("scorer totals score mismatch")
    return results


def _scorer_args(
    scorer: str | Iterable[str],
    model: Path,
    language: str,
    paths: list[Path],
    scoring: dict[str, Any],
) -> list[str]:
    command = [scorer] if isinstance(scorer, str) else list(scorer)
    command += [
        "--model",
        str(model),
        "--lang",
        language,
        "--format",
        "jsonl",
        "--progress",
        "never",
        "--no-download",
        "--no-ignore",
        "--include-headers",
    ]
    argv = scoring.get("argv")
    if argv:
        if not isinstance(argv, list) or not all(
            isinstance(value, str) for value in argv
        ):
            raise WorkerError("profile scoring.argv must be a string array")
        command += argv
    command += [str(path) for path in paths]
    return command


_EXTENSIONS = {
    "c": ".c",
    "cpp": ".cc",
    "csharp": ".cs",
    "go": ".go",
    "java": ".java",
    "javascript": ".js",
    "python": ".py",
    "rust": ".rs",
}


def _materialize_inputs(items: list[dict[str, Any]], directory: Path) -> None:
    """Give immutable Git blobs ordinary language-bearing file names.

    Discovery is intentionally left enabled even with a forced language, so
    extensionless blob object names would otherwise be skipped by the CLI.
    """
    directory.mkdir(parents=True, exist_ok=True)
    for item in items:
        target = directory / (item["key"] + _EXTENSIONS.get(item["language"], ".txt"))
        item["_original_input"] = item["_input"]
        try:
            os.link(item["_input"], target)
        except OSError:
            target.write_bytes(Path(item["_input"]).read_bytes())
        item["_input"] = target.resolve()


def _run_worker(
    plan_path: str | os.PathLike[str],
    worker_id: int,
    store: Any,
    output_dir: str | os.PathLike[str],
    scorer: str | Iterable[str],
    model: str | os.PathLike[str],
    installed_root: str | os.PathLike[str],
    deadline_seconds: int = 6600,
) -> dict[str, Any]:
    """Execute a worker assignment and always persist its artifact."""
    started, output = time.monotonic(), Path(output_dir)
    deadline = started + deadline_seconds
    artifact = {
        "schema_version": 1,
        "identity": None,
        "fingerprint": None,
        "worker_id": worker_id,
        "status": "failed",
        "results": {},
        "errors": [],
        "elapsed_seconds": 0.0,
        "invocations": [],
    }
    previous_sigterm = signal.signal(
        signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt())
    )
    try:
        plan = json.loads(Path(plan_path).read_text(encoding="utf-8"))
        if not isinstance(plan, dict):
            raise WorkerError("plan is not an object")
        artifact["identity"], artifact["fingerprint"] = (
            plan.get("identity"),
            plan.get("fingerprint"),
        )
        if plan.get("schema_version") != 1:
            raise WorkerError("unsupported plan schema")
        expected_fingerprint = hashlib.sha256(
            _json(
                {
                    "scoring": plan["profile"]["scoring"],
                    "build": plan["profile"]["build"],
                }
            )
        ).hexdigest()
        if plan.get("fingerprint") != expected_fingerprint:
            raise WorkerError("plan fingerprint does not match profile")
        assignment = next(
            (
                item
                for item in plan.get("workers", [])
                if item.get("worker_id") == worker_id
            ),
            None,
        )
        if assignment is None:
            raise WorkerError("worker is absent from plan")
        build = plan["profile"]["build"]
        root, model_path = Path(installed_root).resolve(), Path(model)
        execution_host = build.get("execution_host")
        if (build.get("execution_image") == "none") != (execution_host is not None):
            raise WorkerError("bare-host execution requires a complete host identity")
        gpu_environment = (
            _host_gpu_environment(execution_host, deadline)
            if execution_host is not None
            else {}
        )
        if (
            not build.get("installed_files")
            or not build.get("inference_abi")
            or not build.get("source_commit")
            or not build.get("execution_image")
        ):
            raise WorkerError("build identity is incomplete")
        for relative, digest in build.get("installed_files", {}).items():
            candidate = (root / relative).resolve()
            if root not in candidate.parents:
                raise WorkerError("installed scorer path escapes installed root")
            if not candidate.is_file() or _hash(candidate, deadline) != digest:
                raise WorkerError(
                    "installed scorer file checksum mismatch: %s" % relative
                )
        if not model_path.is_file():
            raise WorkerError("model file is missing")
        expected_model_bytes = build.get("model_bytes")
        if (
            model_path.stat().st_size != expected_model_bytes
            or _hash(model_path, deadline) != build.get("model_sha256")
            or model_path.stat().st_size != expected_model_bytes
        ):
            raise WorkerError("model digest or size mismatch")
        items = []
        plan_root = Path(plan_path).parent.resolve()
        for key in assignment.get("keys", []):
            item = dict(plan["items"].get(key) or {})
            if not item or item.get("key") != key:
                raise WorkerError("assignment references unknown item")
            expected_key = hashlib.sha256(
                _json(
                    [
                        item.get("content_sha256"),
                        item.get("language"),
                        plan["fingerprint"],
                    ]
                )
            ).hexdigest()
            if key != expected_key:
                raise WorkerError("item key does not match provenance")
            relative_blob = Path(item["blob"])
            if relative_blob.is_absolute() or ".." in relative_blob.parts:
                raise WorkerError("input blob path escapes plan")
            source_blob = plan_root / relative_blob
            if any(
                parent.is_symlink()
                for parent in [source_blob, *source_blob.parents]
                if parent != plan_root and plan_root in parent.parents
            ):
                raise WorkerError("input blob must not traverse symlinks")
            blob = source_blob.resolve()
            if plan_root not in blob.parents:
                raise WorkerError("input blob path escapes plan")
            if (
                not blob.is_file()
                or blob.is_symlink()
                or _hash(blob, deadline) != item["content_sha256"]
                or blob.stat().st_size != item.get("size")
            ):
                raise WorkerError("input blob checksum or size mismatch")
            item["_input"] = blob.resolve()
            items.append(item)
        if time.monotonic() > deadline:
            raise WorkerError("worker deadline exceeded during verification")
        executable = Path(
            (scorer if isinstance(scorer, str) else list(scorer)[0])
        ).resolve()
        if (
            root not in executable.parents
            or not executable.is_file()
            or not os.access(executable, os.X_OK)
        ):
            raise WorkerError("scorer executable is outside installed root")
        relative_executable = executable.relative_to(root).as_posix()
        if (
            relative_executable not in build["installed_files"]
            or _hash(executable, deadline)
            != build["installed_files"][relative_executable]
        ):
            raise WorkerError(
                "scorer executable is not the exact checksum-pinned installed file"
            )
        cache = ResultCache(store)
        collected: list[dict[str, Any]] = []
        with tempfile.TemporaryDirectory(
            prefix="llm-cc-worker-%s-entropy-" % worker_id
        ) as entropy_dir:
            entropy_root = Path(entropy_dir)
            native_dir = entropy_root / "v2" / "entropy"
            native_dir.mkdir(parents=True)
            for name, contents in cache.native_entropy(plan["fingerprint"]).items():
                if time.monotonic() > deadline:
                    raise WorkerError(
                        "worker deadline exceeded during native cache restore"
                    )
                destination = native_dir / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(contents)
            _materialize_inputs(items, entropy_root / "inputs")

            def score(lock_fd=None):
                _run_assignments(
                    plan,
                    items,
                    scorer,
                    model_path,
                    deadline,
                    artifact,
                    collected,
                    entropy_root,
                    output,
                    root,
                    gpu_environment,
                    lock_fd,
                )

            if execution_host is not None:
                with _gpu_lease(execution_host["resource_id"], deadline) as lock_fd:
                    score(lock_fd)
            else:
                score()
            if time.monotonic() > deadline:
                raise WorkerError("worker deadline exceeded before publication")
            for entry in native_dir.rglob("*.cbor"):
                if time.monotonic() > deadline:
                    raise WorkerError(
                        "worker deadline exceeded during native cache publication"
                    )
                if entry.is_file() and not entry.is_symlink():
                    cache.put_native_entropy(
                        plan["fingerprint"],
                        entry.relative_to(native_dir).as_posix(),
                        entry.read_bytes(),
                    )
        # Publish only after every language invocation was completely validated.
        for result in collected:
            if time.monotonic() > deadline:
                raise WorkerError("worker deadline exceeded during publication")
            cache.put(result)
        artifact["results"] = {result["key"]: result for result in collected}
        artifact["status"] = "complete"
    except BaseException as error:
        artifact["errors"].append(str(error))
    artifact["elapsed_seconds"] = time.monotonic() - started
    try:
        _write_artifact(output, artifact)
    finally:
        signal.signal(signal.SIGTERM, previous_sigterm)
    return artifact


def _run_assignments(
    plan: dict[str, Any],
    items: list[dict[str, Any]],
    scorer: str | Iterable[str],
    model_path: Path,
    deadline: float,
    artifact: dict[str, Any],
    collected: list[dict[str, Any]],
    entropy_root: Path,
    output_dir: Path,
    installed_root: Path,
    gpu_environment: dict[str, str] | None = None,
    lock_fd: int | None = None,
) -> None:
    def stop_group(process):
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            pass
        # The leader can exit while a descendant keeps the pipes open. Always
        # kill the remaining group before joining the log readers.
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=5)

    # Each invocation receives one language, so its model session can score
    # all that language's misses in one batch.
    profile, build = plan["profile"], plan["profile"]["build"]
    for language in sorted({item["language"] for item in items}):
        selected = [item for item in items if item["language"] == language]
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise WorkerError("worker deadline exceeded")
        command = _scorer_args(
            scorer,
            model_path,
            language,
            [item["_input"] for item in selected],
            profile["scoring"],
        )
        output_dir.mkdir(parents=True, exist_ok=True)
        safe_language = "".join(char if char.isalnum() else "_" for char in language)
        stdout_path = output_dir / (
            "worker-%s-%s.jsonl" % (artifact["worker_id"], safe_language)
        )
        stderr_path = output_dir / (
            "worker-%s-%s.stderr" % (artifact["worker_id"], safe_language)
        )
        invocation = {
            "language": language,
            "jsonl": stdout_path.name,
            "stderr": stderr_path.name,
            "status": "failed",
        }
        artifact["invocations"].append(invocation)
        try:
            environment = os.environ.copy()
            if gpu_environment:
                # A clean environment prevents inherited loader injections and
                # HIP/HSA tuning overrides from defeating the pinned profile.
                environment = {
                    "PATH": "/usr/bin:/bin",
                    "LANG": "C.UTF-8",
                    "HOME": str(entropy_root),
                }
                environment.update(gpu_environment)
            # Explicit backend directories take precedence over the verified
            # installed bundle, even when their payload is absent from its identity.
            environment.pop("LLM_CC_BACKEND_DIR", None)
            environment["LLM_CC_ENTROPY_CACHE_DIR"] = str(entropy_root)
            environment["LLM_CC_CACHE_DIR"] = str(entropy_root / "models")
            environment["LLM_CC_RUNTIME_DIR"] = str(installed_root / "lib" / "llm-cc")
            process = subprocess.Popen(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                start_new_session=True,
                env=environment,
                **({"pass_fds": (lock_fd,)} if lock_fd is not None else {}),
            )

            def copy_stream(source: Any, destination: Path, tee: bool) -> None:
                with destination.open("wb") as target:
                    read = getattr(source, "read1", source.read)
                    for chunk in iter(lambda: read(64 * 1024), b""):
                        target.write(chunk)
                        target.flush()
                        if tee:
                            try:
                                sys.stderr.buffer.write(chunk)
                                sys.stderr.buffer.flush()
                            except Exception:
                                pass
                try:
                    source.close()
                except Exception:
                    pass

            out_thread = threading.Thread(
                target=copy_stream,
                args=(process.stdout, stdout_path, False),
                daemon=True,
            )
            err_thread = threading.Thread(
                target=copy_stream,
                args=(process.stderr, stderr_path, True),
                daemon=True,
            )
            out_thread.start()
            err_thread.start()
            try:
                process.wait(timeout=remaining)
            except subprocess.TimeoutExpired:
                stop_group(process)
                out_thread.join()
                err_thread.join()
                raise WorkerError("scorer deadline exceeded")
            except BaseException:
                # Includes cancellation and the POSIX wall-clock deadline.
                # Reap the whole scorer group before releasing its GPU lease.
                stop_group(process)
                out_thread.join(timeout=5)
                err_thread.join(timeout=5)
                raise
        except OSError as error:
            raise WorkerError("cannot start scorer: %s" % error) from error
        out_thread.join(timeout=1)
        err_thread.join(timeout=1)
        if out_thread.is_alive() or err_thread.is_alive():
            stop_group(process)
            out_thread.join(timeout=5)
            err_thread.join(timeout=5)
            raise WorkerError("scorer left descendant processes holding its logs open")
        stdout, stderr = stdout_path.read_bytes(), stderr_path.read_bytes()
        if process.returncode != 0:
            raise WorkerError(
                "scorer failed with exit %s: %s"
                % (process.returncode, stderr.decode("utf-8", "replace")[-2000:])
            )
        collected.extend(
            _validate_stream(
                stdout,
                selected,
                plan["fingerprint"],
                profile["scoring"],
                build.get("model_sha256"),
                build.get("model_bytes"),
            )
        )
        invocation["status"] = "complete"


def run_worker(
    plan_path: str | os.PathLike[str],
    worker_id: int,
    store: Any,
    output_dir: str | os.PathLike[str],
    scorer: str | Iterable[str],
    model: str | os.PathLike[str],
    installed_root: str | os.PathLike[str],
    deadline_seconds: int = 6600,
) -> dict[str, Any]:
    """Run with a POSIX wall-clock timer; non-POSIX callers use checks inside."""
    try:
        with Deadline(deadline_seconds):
            return _run_worker(
                plan_path,
                worker_id,
                store,
                output_dir,
                scorer,
                model,
                installed_root,
                deadline_seconds,
            )
    except DeadlineExceeded as error:
        artifact = {
            "schema_version": 1,
            "identity": None,
            "fingerprint": None,
            "worker_id": worker_id,
            "status": "failed",
            "results": {},
            "errors": [str(error)],
            "elapsed_seconds": 0.0,
            "invocations": [],
        }
        _write_artifact(Path(output_dir), artifact)
        return artifact
