import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from .cache import CacheError, FilesystemStore, ResultCache, S3Store
from .common import digest
from .worker import WorkerError, _validate_stream


class StrictStreamTest(unittest.TestCase):
    def setUp(self):
        self.expected = [
            {
                "key": "key",
                "content_sha256": "sha",
                "language": "python",
                "_input": Path("/input.py"),
            }
        ]
        self.scoring = {
            "expected_configuration": {"context": 131072, "tau_percentile": None}
        }
        self.events = [
            {
                "type": "configuration",
                "context": 131072,
                "tau_percentile": None,
                "model_sha256": "model",
                "model_size": 5,
                "language": "python",
                "no_ignore": True,
                "no_download": True,
                "include_headers": True,
            },
            {
                "type": "file",
                "path": "/input.py",
                "language": "python",
                "llm_cc": 2.0,
                "token_count": 3,
            },
            {
                "type": "totals",
                "partial": False,
                "failed": 0,
                "discovered": 1,
                "analyzed": 1,
                "llm_cc": 2.0,
                "token_count": 3,
            },
        ]

    def validate(self, events):
        raw = ("\n".join(json.dumps(x) for x in events) + "\n").encode()
        return _validate_stream(raw, self.expected, "fp", self.scoring, "model", 5)

    def test_valid_and_each_invalid_field(self):
        self.assertEqual(self.validate(self.events)[0]["llm_cc"], 2)
        for index, field, value in [
            (0, "context", 2048),
            (0, "model_sha256", "other"),
            (0, "model_size", 6),
            (0, "no_download", False),
            (0, "no_ignore", 1),
            (0, "language", "cpp"),
            (1, "language", "cpp"),
            (1, "path", "/unexpected.py"),
            (1, "llm_cc", float("nan")),
            (1, "llm_cc", float("inf")),
            (1, "llm_cc", -1),
            (1, "token_count", True),
            (1, "token_count", -1),
            (2, "failed", False),
            (2, "analyzed", True),
            (2, "discovered", 2),
            (2, "partial", True),
            (2, "token_count", 5),
            (2, "llm_cc", True),
            (2, "llm_cc", 9),
        ]:
            with self.subTest(index=index, field=field, value=value):
                events = copy.deepcopy(self.events)
                events[index][field] = value
                with self.assertRaises(WorkerError):
                    self.validate(events)

    def test_missing_duplicate_error_and_unordered_events(self):
        variants = [
            self.events[:-1],
            self.events[1:],
            [self.events[0], self.events[-1]],
            [*self.events[:-1], self.events[1], self.events[-1]],
            [*self.events, self.events[-1]],
            [self.events[0], {"type": "error", "message": "oops"}, *self.events[1:]],
            [self.events[1], self.events[0], self.events[2]],
        ]
        for events in variants:
            with self.subTest(events=events), self.assertRaises(WorkerError):
                self.validate(events)
        for index, field in [(0, "tau_percentile"), (2, "partial"), (2, "failed")]:
            events = copy.deepcopy(self.events)
            del events[index][field]
            with self.subTest(missing=field), self.assertRaises(WorkerError):
                self.validate(events)


class StoreFailureTest(unittest.TestCase):
    class StorageError(Exception):
        def __init__(self, code):
            self.response = {"Error": {"Code": code}}

    def test_s3_absent_object_is_miss_but_auth_and_transport_are_errors(self):
        for code in [
            "NoSuchKey",
            "403",
            "AccessDenied",
            "InternalError",
            "ExpiredToken",
        ]:
            store = S3Store.__new__(S3Store)
            store.bucket, store.prefix = "bucket", "prefix"
            store._errors = (self.StorageError,)

            class Client:
                def get_object(inner, **kwargs):
                    raise self.StorageError(code)

            store.client = Client()
            with self.subTest(code=code):
                if code == "NoSuchKey":
                    self.assertIsNone(store.get("results/key.json"))
                else:
                    with self.assertRaises(CacheError):
                        store.get("results/key.json")

    def test_corruption_schema_and_permission_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            store = FilesystemStore(directory)
            cache = ResultCache(store)
            fp, sha = "f" * 64, "a" * 64
            item = {
                "key": digest([sha, "python", fp]),
                "content_sha256": sha,
                "language": "python",
            }
            result = dict(
                item, schema_version=1, fingerprint=fp, llm_cc=2.0, token_count=3
            )
            for bad in [
                b"\xff",
                b"null",
                b"[]",
                b'{"result": null}',
                b'{"result": []}',
            ]:
                store.put("results/" + item["key"] + ".json", bad)
                self.assertIsNone(cache.get(item, fp))
            cache.put(result)
            self.assertEqual(cache.get(item, fp), result)
            envelope = json.loads(store.get("results/" + item["key"] + ".json"))
            envelope["result"]["schema_version"] = 2
            encoded = json.dumps(
                envelope["result"], sort_keys=True, separators=(",", ":")
            ).encode()
            envelope["sha256"] = hashlib.sha256(encoded).hexdigest()
            store.put("results/" + item["key"] + ".json", json.dumps(envelope).encode())
            self.assertIsNone(cache.get(item, fp))

            class BrokenStore:
                def get(self, key):
                    raise PermissionError("denied")

            with self.assertRaises(PermissionError):
                ResultCache(BrokenStore()).get(item, fp)


if __name__ == "__main__":
    unittest.main()
