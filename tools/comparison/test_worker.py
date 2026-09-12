import hashlib
import io
import json
import os
import subprocess
import sys
import time
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools.comparison.cache import FilesystemStore, ResultCache
from tools.comparison.worker import (
    WorkerError,
    _gpu_lease,
    _host_gpu_environment,
    run_worker,
)


class WorkerTest(unittest.TestCase):
    def _plan(self, root):
        blob = Path(root) / "blobs" / ("a" * 64)
        blob.parent.mkdir()
        blob.write_text("x = 1\n")
        source = hashlib.sha256(blob.read_bytes()).hexdigest()
        model = Path(root) / "model.gguf"
        model.write_bytes(b"model")
        binary = Path(root) / "llm-cc"
        binary.write_bytes(b"binary")
        binary.chmod(0o755)
        profile = {
            "scoring": {"expected_configuration": {"analysis_version": 2}},
            "build": {
                "installed_files": {"llm-cc": hashlib.sha256(b"binary").hexdigest()},
                "model_sha256": hashlib.sha256(b"model").hexdigest(),
                "model_bytes": 5,
                "source_commit": "4646123",
                "inference_abi": "test",
                "execution_image": "test-image",
            },
        }
        fp = hashlib.sha256(
            json.dumps(profile, separators=(",", ":"), sort_keys=True).encode()
        ).hexdigest()
        key = hashlib.sha256(
            json.dumps(
                [source, "python", fp], separators=(",", ":"), sort_keys=True
            ).encode()
        ).hexdigest()
        plan = {
            "schema_version": 1,
            "identity": {"pipeline_id": "p"},
            "fingerprint": fp,
            "profile": profile,
            "items": {
                key: {
                    "key": key,
                    "content_sha256": source,
                    "language": "python",
                    "size": blob.stat().st_size,
                    "blob": "blobs/" + "a" * 64,
                }
            },
            "workers": [{"worker_id": 0, "keys": [key], "bytes": 6}],
        }
        path = Path(root) / "plan.json"
        path.write_text(json.dumps(plan))
        return path, model, binary, key, blob

    def test_zero_deadline_still_writes_failure_artifact(self):
        with tempfile.TemporaryDirectory() as directory:
            plan, model, binary, key, blob = self._plan(directory)
            result = run_worker(
                plan,
                0,
                FilesystemStore(Path(directory) / "cache"),
                Path(directory) / "out",
                str(binary),
                model,
                directory,
                deadline_seconds=0,
            )
            self.assertEqual(result["status"], "failed")
            self.assertTrue((Path(directory) / "out" / "worker-0.json").is_file())

    def test_complete_stream_is_cached_and_artifact_written(self):
        with tempfile.TemporaryDirectory() as directory:
            plan, model, binary, key, blob = self._plan(directory)
            stream = (
                "\n".join(
                    json.dumps(v)
                    for v in [
                        {
                            "type": "configuration",
                            "analysis_version": 2,
                            "model_sha256": hashlib.sha256(b"model").hexdigest(),
                            "model_size": 5,
                            "language": "python",
                            "no_download": True,
                            "no_ignore": True,
                            "include_headers": True,
                        },
                        {
                            "type": "file",
                            "path": str(blob.resolve()),
                            "language": "python",
                            "llm_cc": 3.0,
                            "token_count": 4,
                        },
                        {
                            "type": "totals",
                            "discovered": 1,
                            "analyzed": 1,
                            "failed": 0,
                            "partial": False,
                            "llm_cc": 3.0,
                            "token_count": 4,
                        },
                    ]
                )
                + "\n"
            )

            class Process:
                pid = os.getpid()
                returncode = 0
                stdout, stderr = io.BytesIO(stream.encode()), io.BytesIO()

                def communicate(self, timeout=None):
                    return stream.encode(), b""

                def wait(self, timeout=None):
                    return 0

            with (
                patch.dict(
                    os.environ,
                    {
                        "LLM_CC_BACKEND_DIR": "/unverified/backend",
                        "CUDA_VISIBLE_DEVICES": "2",
                    },
                ),
                patch(
                    "tools.comparison.worker.subprocess.Popen", return_value=Process()
                ) as popen,
            ):
                result = run_worker(
                    plan,
                    0,
                    FilesystemStore(Path(directory) / "cache"),
                    Path(directory) / "out",
                    str(binary),
                    model,
                    directory,
                )
            self.assertEqual(result["status"], "complete")
            environment = popen.call_args.kwargs["env"]
            self.assertNotIn("LLM_CC_BACKEND_DIR", environment)
            self.assertEqual(environment["CUDA_VISIBLE_DEVICES"], "2")
            self.assertIn(key, result["results"])
            self.assertTrue((Path(directory) / "out" / "worker-0.json").exists())

    def test_wall_clock_exception_kills_scorer_group_before_returning(self):
        from tools.comparison.deadline import DeadlineExceeded

        with tempfile.TemporaryDirectory() as directory:
            plan, model, binary, key, blob = self._plan(directory)

            class Process:
                pid = 999999
                returncode = None
                stdout, stderr = io.BytesIO(), io.BytesIO()

                def wait(self, timeout=None):
                    if timeout not in (15, 5):
                        raise DeadlineExceeded("deadline exceeded")
                    return 0

            with (
                patch(
                    "tools.comparison.worker.subprocess.Popen", return_value=Process()
                ),
                patch("tools.comparison.worker.os.killpg") as kill,
            ):
                result = run_worker(
                    plan,
                    0,
                    FilesystemStore(Path(directory) / "cache"),
                    Path(directory) / "out",
                    str(binary),
                    model,
                    directory,
                )
            self.assertEqual(result["status"], "failed")
            self.assertEqual(result["results"], {})
            self.assertEqual(
                [call.args[1] for call in kill.call_args_list],
                [__import__("signal").SIGTERM, __import__("signal").SIGKILL],
            )

    def test_truncated_stream_is_failed_and_not_published(self):
        with tempfile.TemporaryDirectory() as directory:
            plan, model, binary, key, blob = self._plan(directory)

            class Process:
                pid = os.getpid()
                returncode = 0
                stdout, stderr = io.BytesIO(b'{"type":"configuration"}'), io.BytesIO()

                def communicate(self, timeout=None):
                    return b'{"type":"configuration"}', b""

                def wait(self, timeout=None):
                    return 0

            with patch(
                "tools.comparison.worker.subprocess.Popen", return_value=Process()
            ):
                result = run_worker(
                    plan,
                    0,
                    FilesystemStore(Path(directory) / "cache"),
                    Path(directory) / "out",
                    str(binary),
                    model,
                    directory,
                )
            self.assertEqual(result["status"], "failed")
            self.assertEqual(result["results"], {})

    def test_bad_totals_are_rejected_before_publication(self):
        with tempfile.TemporaryDirectory() as directory:
            plan, model, binary, key, blob = self._plan(directory)
            stream = (
                "\n".join(
                    json.dumps(v)
                    for v in [
                        {
                            "type": "configuration",
                            "analysis_version": 2,
                            "model_sha256": hashlib.sha256(b"model").hexdigest(),
                            "model_size": 5,
                            "language": "python",
                            "no_download": True,
                            "no_ignore": True,
                            "include_headers": True,
                        },
                        {
                            "type": "file",
                            "path": str(blob.resolve()),
                            "language": "python",
                            "llm_cc": 3.0,
                            "token_count": 4,
                        },
                        {
                            "type": "totals",
                            "discovered": 1,
                            "analyzed": 1,
                            "failed": 0,
                            "partial": False,
                            "llm_cc": 2.0,
                            "token_count": 4,
                        },
                    ]
                )
                + "\n"
            )

            class Process:
                pid = os.getpid()
                returncode = 0
                stdout, stderr = io.BytesIO(stream.encode()), io.BytesIO()

                def communicate(self, timeout=None):
                    return stream.encode(), b""

                def wait(self, timeout=None):
                    return 0

            with patch(
                "tools.comparison.worker.subprocess.Popen", return_value=Process()
            ):
                result = run_worker(
                    plan,
                    0,
                    FilesystemStore(Path(directory) / "cache"),
                    Path(directory) / "out",
                    str(binary),
                    model,
                    directory,
                )
            self.assertEqual(result["status"], "failed")
            self.assertFalse(
                list((Path(directory) / "cache").rglob("*"))
                if (Path(directory) / "cache").exists()
                else []
            )

    def test_real_scorer_receives_private_caches_and_publishes_native_entries(self):
        with tempfile.TemporaryDirectory() as directory:
            plan_path, model, binary, old_key, blob = self._plan(directory)
            digest = hashlib.sha256(b"model").hexdigest()
            script = (
                """#!/usr/bin/env python3
import json, os, pathlib, sys
source = sys.argv[-1]
base = pathlib.Path(os.environ['LLM_CC_ENTROPY_CACHE_DIR']) / 'v2/entropy/nested'
base.mkdir(parents=True, exist_ok=True); (base / 'entry.cbor').write_bytes(b'cbor')
print(json.dumps({'type':'configuration','analysis_version':2,'model_sha256':'%s','model_size':5,'language':'python','no_download':True,'no_ignore':True,'include_headers':True}))
print(json.dumps({'type':'file','path':source,'language':'python','llm_cc':1.0,'token_count':1}))
print(json.dumps({'type':'totals','discovered':1,'analyzed':1,'failed':0,'partial':False,'llm_cc':1.0,'token_count':1}))
"""
                % digest
            )
            binary.write_text(script)
            binary.chmod(0o755)
            plan = json.loads(plan_path.read_text())
            profile = plan["profile"]
            profile["build"]["installed_files"]["llm-cc"] = hashlib.sha256(
                binary.read_bytes()
            ).hexdigest()
            fingerprint = hashlib.sha256(
                json.dumps(profile, sort_keys=True, separators=(",", ":")).encode()
            ).hexdigest()
            item = plan["items"].pop(old_key)
            new_key = hashlib.sha256(
                json.dumps(
                    [item["content_sha256"], "python", fingerprint],
                    sort_keys=True,
                    separators=(",", ":"),
                ).encode()
            ).hexdigest()
            item["key"] = new_key
            plan["items"][new_key] = item
            plan["workers"][0]["keys"] = [new_key]
            plan["fingerprint"] = fingerprint
            plan_path.write_text(json.dumps(plan))
            result = run_worker(
                plan_path,
                0,
                FilesystemStore(Path(directory) / "cache"),
                Path(directory) / "out",
                str(binary),
                model,
                directory,
            )
            self.assertEqual(result["status"], "complete")
            self.assertEqual(
                ResultCache(FilesystemStore(Path(directory) / "cache")).native_entropy(
                    fingerprint
                ),
                {"nested/entry.cbor": b"cbor"},
            )
            self.assertTrue(
                (Path(directory) / "out" / "worker-0-python.jsonl").is_file()
            )
            self.assertTrue(
                (Path(directory) / "out" / "worker-0-python.stderr").is_file()
            )

    def test_timeout_retains_partial_stream_files(self):
        with tempfile.TemporaryDirectory() as directory:
            plan, model, binary, key, blob = self._plan(directory)

            class Process:
                pid = 999999
                returncode = -15
                stdout, stderr = (
                    io.BytesIO(b'{"partial":true}\n'),
                    io.BytesIO(b"progress\n"),
                )

                def wait(self, timeout=None):
                    if timeout not in (15, 5):
                        raise __import__("subprocess").TimeoutExpired("fake", timeout)
                    return -15

            with (
                patch(
                    "tools.comparison.worker.subprocess.Popen", return_value=Process()
                ),
                patch("tools.comparison.worker.os.killpg"),
            ):
                result = run_worker(
                    plan,
                    0,
                    FilesystemStore(Path(directory) / "cache"),
                    Path(directory) / "out",
                    str(binary),
                    model,
                    directory,
                    deadline_seconds=1,
                )
            self.assertEqual(result["status"], "failed")
            self.assertIn(
                "progress",
                (Path(directory) / "out" / "worker-0-python.stderr").read_text(),
            )
            self.assertIn(
                "partial",
                (Path(directory) / "out" / "worker-0-python.jsonl").read_text(),
            )


@unittest.skipUnless(
    sys.platform == "linux", "KFD topology and GPU leases require Linux"
)
class HostGpuTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.sysfs = self.root / "sys"
        self.devices = self.root / "dev"
        self.pci = self.sysfs / "bus/pci/devices/0000:03:00.0"
        self.pci.mkdir(parents=True)
        (self.pci / "vendor").write_text("0x1002")
        (self.pci / "mem_info_vram_total").write_text(str(24 * 1024**3))
        node = self.sysfs / "class/kfd/kfd/topology/nodes/1"
        node.mkdir(parents=True)
        self.properties = node / "properties"
        self.properties.write_text(
            "vendor_id 4098\ndomain 0\nlocation_id 768\ngfx_target_version 110000\nunique_id 2408463410737365011\ndrm_render_minor 128\n"
        )
        (self.devices / "dri").mkdir(parents=True)
        (self.devices / "kfd").touch()
        (self.devices / "dri/renderD128").touch()
        runtime = self.root / "lib.so"
        runtime.write_bytes(b"runtime")
        self.host = {
            "gpu_vendor": "amd",
            "gpu_pci_address": "0000:03:00.0",
            "gpu_arch": "gfx1100",
            "gpu_vram_bytes_min": 24 * 1024**3,
            "resource_id": "bazzite-radeon-0",
            "runtime_files": {str(runtime): hashlib.sha256(b"runtime").hexdigest()},
        }

    def verify(self):
        with patch("tools.comparison.worker.stat.S_ISCHR", return_value=True):
            return _host_gpu_environment(
                self.host, time.monotonic() + 10, self.sysfs, self.devices
            )

    def test_actual_gpu_uuid_selected_and_wrong_hardware_fails_closed(self):
        env = self.verify()
        self.assertEqual(env["ROCR_VISIBLE_DEVICES"], "GPU-216c94b62386c013")
        self.assertEqual(env["HIP_VISIBLE_DEVICES"], "0")
        for field, value in (
            ("gpu_arch", "gfx1036"),
            ("gpu_vram_bytes_min", 25 * 1024**3),
            ("gpu_pci_address", "0000:0e:00.0"),
        ):
            with self.subTest(field=field):
                old = self.host[field]
                self.host[field] = value
                with self.assertRaises((WorkerError, OSError)):
                    self.verify()
                self.host[field] = old
        self.properties.write_text(
            self.properties.read_text().replace(
                "unique_id 2408463410737365011", "unique_id 0"
            )
        )
        with self.assertRaisesRegex(WorkerError, "UUID"):
            self.verify()

    def test_runtime_change_or_device_permission_fails_before_inference(self):
        with patch("tools.comparison.worker.os.access", return_value=False):
            with self.assertRaisesRegex(WorkerError, "readable and writable"):
                self.verify()
        (self.root / "lib.so").write_bytes(b"changed")
        with self.assertRaisesRegex(WorkerError, "runtime checksum mismatch"):
            self.verify()

    def test_shared_lock_serializes_and_survives_parent_release_while_child_runs(self):
        patcher = patch("tools.comparison.worker._validate_lock_directory")
        patcher.start()
        self.addCleanup(patcher.stop)
        lock_root = self.root / "locks"
        lock_root.mkdir(mode=0o755)
        (lock_root / "bazzite-radeon-0.lock").touch()
        deadline = time.monotonic() + 5
        with _gpu_lease("bazzite-radeon-0", deadline, lock_root) as fd:
            with self.assertRaisesRegex(WorkerError, "waiting for shared GPU"):
                with _gpu_lease("bazzite-radeon-0", time.monotonic() + 0.03, lock_root):
                    self.fail("concurrent GPU lease")
            child = subprocess.Popen(["sleep", "0.2"], pass_fds=(fd,))
        try:
            with self.assertRaisesRegex(WorkerError, "waiting for shared GPU"):
                with _gpu_lease("bazzite-radeon-0", time.monotonic() + 0.03, lock_root):
                    self.fail("released child GPU lease")
        finally:
            child.wait(timeout=5)
        with _gpu_lease("bazzite-radeon-0", deadline, lock_root):
            pass
        with self.assertRaisesRegex(WorkerError, "preprovision"):
            with _gpu_lease("missing", deadline, lock_root):
                pass
        (lock_root / "alias.lock").symlink_to(lock_root / "bazzite-radeon-0.lock")
        with self.assertRaisesRegex(WorkerError, "preprovision"):
            with _gpu_lease("alias", deadline, lock_root):
                pass
