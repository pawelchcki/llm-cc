import hashlib
import json
import tempfile
import threading
import unittest
from pathlib import Path

from tools.comparison.cache import FilesystemStore, ResultCache


def item():
    fingerprint = "f" * 64
    content = "a" * 64
    key = hashlib.sha256(
        json.dumps(
            [content, "python", fingerprint], separators=(",", ":"), sort_keys=True
        ).encode()
    ).hexdigest()
    return {"key": key, "content_sha256": content, "language": "python"}, fingerprint


class CacheTest(unittest.TestCase):
    def test_result_round_trip_and_corruption_is_miss(self):
        with tempfile.TemporaryDirectory() as directory:
            value, fingerprint = item()
            cache = ResultCache(FilesystemStore(directory))
            result = {
                "schema_version": 1,
                **value,
                "fingerprint": fingerprint,
                "llm_cc": 1.5,
                "token_count": 2,
            }
            cache.put(result)
            self.assertEqual(cache.get(value, fingerprint), result)
            path = next(Path(directory).rglob("*.json"))
            path.write_text("broken")
            self.assertIsNone(cache.get(value, fingerprint))
            path.write_bytes(b"\xff")
            self.assertIsNone(cache.get(value, fingerprint))

    def test_provenance_mismatch_is_miss(self):
        with tempfile.TemporaryDirectory() as directory:
            value, fingerprint = item()
            store = FilesystemStore(directory)
            cache = ResultCache(store)
            result = {
                "schema_version": 1,
                **value,
                "fingerprint": fingerprint,
                "llm_cc": 1,
                "token_count": 2,
            }
            cache.put(result)
            other = dict(value)
            other["language"] = "go"
            self.assertIsNone(cache.get(other, fingerprint))

    def test_malformed_result_fields_are_cache_misses(self):
        with tempfile.TemporaryDirectory() as directory:
            value, fingerprint = item()
            cache = ResultCache(FilesystemStore(directory))
            result = {
                "schema_version": 1,
                **value,
                "fingerprint": fingerprint,
                "llm_cc": 1,
                "token_count": 2,
            }
            for timestamp in (None, 123, [], {}):
                with self.subTest(timestamp=timestamp):
                    cache.put(result)
                    path = next(Path(directory).rglob("*.json"))
                    envelope = json.loads(path.read_bytes())
                    envelope["stored_at"] = timestamp
                    path.write_text(json.dumps(envelope))
                    self.assertIsNone(cache.get(value, fingerprint))
            cache.put(result)
            envelope = json.loads(path.read_bytes())
            envelope["result"]["llm_cc"] = 10**400
            envelope["sha256"] = hashlib.sha256(
                json.dumps(
                    envelope["result"], sort_keys=True, separators=(",", ":")
                ).encode()
            ).hexdigest()
            path.write_text(json.dumps(envelope))
            self.assertIsNone(cache.get(value, fingerprint))

    def test_entropy_entries_have_separate_namespace(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = ResultCache(FilesystemStore(directory))
            cache.put_entropy("one", b"native")
            self.assertEqual(cache.get_entropy("one"), b"native")
            self.assertTrue((Path(directory) / "entropy" / "one").exists())

    def test_concurrent_atomic_writes_leave_a_valid_entry(self):
        with tempfile.TemporaryDirectory() as directory:
            value, fingerprint = item()
            cache = ResultCache(FilesystemStore(directory))
            result = {
                "schema_version": 1,
                **value,
                "fingerprint": fingerprint,
                "llm_cc": 1,
                "token_count": 2,
            }
            threads = [
                threading.Thread(target=cache.put, args=(result,)) for _ in range(12)
            ]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            self.assertEqual(cache.get(value, fingerprint), result)

    def test_native_entries_are_individually_enveloped(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = ResultCache(FilesystemStore(directory))
            cache.put_native_entropy("f" * 64, "entry.cbor", b"cbor")
            self.assertEqual(cache.native_entropy("f" * 64), {"entry.cbor": b"cbor"})
            path = next(Path(directory).rglob("*.cbor"))
            path.write_text("not-json")
            self.assertEqual(cache.native_entropy("f" * 64), {})

    def test_malformed_native_timestamp_is_cache_miss(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = ResultCache(FilesystemStore(directory))
            for timestamp in (None, 123, [], {}):
                with self.subTest(timestamp=timestamp):
                    cache.put_native_entropy("f" * 64, "entry.cbor", b"cbor")
                    path = next(Path(directory).rglob("*.cbor"))
                    envelope = json.loads(path.read_bytes())
                    envelope["stored_at"] = timestamp
                    path.write_text(json.dumps(envelope))
                    self.assertEqual(cache.native_entropy("f" * 64), {})
