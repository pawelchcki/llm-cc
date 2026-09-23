import hashlib
import io
import json
import os
import stat
import tempfile
import threading
import time
import unittest
from pathlib import Path

from tools.comparison.cache import (
    CacheError,
    FilesystemStore,
    ResultCache,
    S3Store,
    _read_many,
)


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

    @unittest.skipUnless(os.name == "posix", "POSIX shared-cache permissions")
    def test_atomic_writes_honor_shared_group_and_private_umasks(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            root.chmod(0o2770)
            store = FilesystemStore(root)
            for mask, mode in ((0o007, 0o660), (0o077, 0o600), (0o022, 0o640)):
                with self.subTest(umask=oct(mask)):
                    previous = os.umask(mask)
                    try:
                        store.put("pipelines/plan.json", b"first")
                        store.put("pipelines/plan.json", b"replaced")
                    finally:
                        os.umask(previous)
                    target = root / "pipelines/plan.json"
                    self.assertEqual(stat.S_IMODE(target.stat().st_mode), mode)
                    self.assertEqual(target.stat().st_gid, root.stat().st_gid)
                    self.assertEqual(store.get("pipelines/plan.json"), b"replaced")

    def test_native_entries_are_individually_enveloped(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = ResultCache(FilesystemStore(directory))
            cache.put_native_entropy("f" * 64, "entry.cbor", b"cbor")
            self.assertEqual(cache.native_entropy("f" * 64), {"entry.cbor": b"cbor"})
            path = next(Path(directory).rglob("*.cbor"))
            path.write_text("not-json")
            self.assertEqual(cache.native_entropy("f" * 64), {})

    def test_concurrent_native_publication_preserves_every_workers_entries(self):
        # Workers publish individual entries, never a shared archive, so one
        # worker's publication cannot delete or replace another's.
        with tempfile.TemporaryDirectory() as directory:
            barrier = threading.Barrier(4)

            def publish(worker):
                cache = ResultCache(FilesystemStore(directory))
                barrier.wait()
                for index in range(10):
                    name = "v2/entropy/w%d-%d.cbor" % (worker, index)
                    cache.put_native_entropy("f" * 64, name, name.encode())

            threads = [
                threading.Thread(target=publish, args=(worker,)) for worker in range(4)
            ]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            restored = ResultCache(FilesystemStore(directory)).native_entropy("f" * 64)
            self.assertEqual(len(restored), 40)
            self.assertEqual(restored["v2/entropy/w3-9.cbor"], b"v2/entropy/w3-9.cbor")

    def test_native_entries_read_concurrently_and_fail_fast(self):
        class CountingStore(FilesystemStore):
            def __init__(self, root, failing=None):
                super().__init__(root)
                self.failing = failing
                self.lock = threading.Lock()
                self.started = 0
                self.in_flight = 0
                self.max_in_flight = 0

            def get(self, key):
                with self.lock:
                    self.started += 1
                    self.in_flight += 1
                    self.max_in_flight = max(self.max_in_flight, self.in_flight)
                try:
                    time.sleep(0.01)
                    if self.failing and key.endswith(self.failing):
                        raise CacheError("store unavailable")
                    return super().get(key)
                finally:
                    with self.lock:
                        self.in_flight -= 1

        with tempfile.TemporaryDirectory() as directory:
            names = ["entry%d.cbor" % index for index in range(6)]
            writer = ResultCache(FilesystemStore(directory))
            for name in names:
                writer.put_native_entropy("f" * 64, name, name.encode())
            store = CountingStore(directory)
            cache = ResultCache(store)
            self.assertEqual(
                cache.native_entropy("f" * 64, 4),
                {name: name.encode() for name in names},
            )
            self.assertLessEqual(store.max_in_flight, 4)
            self.assertGreater(store.max_in_flight, 1)
            failing = ResultCache(CountingStore(directory, failing="entry0.cbor"))
            with self.assertRaises(CacheError):
                failing.native_entropy("f" * 64, 4)
            for concurrency in (0, 65, True, "4"):
                with self.subTest(concurrency=concurrency):
                    self.assertRaises(
                        ValueError, cache.native_entropy, "f" * 64, concurrency
                    )

    def test_reads_are_streamed_rather_than_accumulated(self):
        started = []

        class Store:
            def get(self, key):
                started.append(key)
                return key.encode()

        stream = _read_many(Store(), ["k%d" % i for i in range(12)], 3)
        first = next(stream)
        # Only the bounded window may have been read before the first result,
        # so a cache near its size limit is never resident all at once.
        self.assertEqual(first, ("k0", b"k0"))
        self.assertLessEqual(len(started), 3)
        self.assertEqual([key for key, _ in stream], ["k%d" % i for i in range(1, 12)])
        self.assertEqual(len(started), 12)

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


class SlowFilesystemStore(FilesystemStore):
    """Widens the read-modify-write window so a missing lock would lose updates."""

    def put(self, key, value):
        time.sleep(0.002)
        super().put(key, value)


class FakeClientError(Exception):
    def __init__(self, code):
        super().__init__(code)
        self.response = {"Error": {"Code": code}}


class FakeS3Client:
    """Conditional PutObject semantics as documented for Amazon S3."""

    def __init__(self):
        self.objects = {}
        self.requests = []

    def get_object(self, Bucket, Key):
        if Key not in self.objects:
            raise FakeClientError("NoSuchKey")
        body, etag = self.objects[Key]
        return {"Body": io.BytesIO(body), "ETag": etag}

    def put_object(self, Bucket, Key, Body, IfMatch=None, IfNoneMatch=None):
        self.requests.append({"IfMatch": IfMatch, "IfNoneMatch": IfNoneMatch})
        current = self.objects.get(Key)
        if IfNoneMatch == "*" and current is not None:
            raise FakeClientError("PreconditionFailed")
        if IfMatch is not None:
            if current is None:
                raise FakeClientError("NoSuchKey")
            if current[1] != IfMatch:
                raise FakeClientError("PreconditionFailed")
        etag = '"%s"' % hashlib.md5(Body).hexdigest()
        self.objects[Key] = (Body, etag)
        return {"ETag": etag}


def fake_s3_store(client):
    store = S3Store.__new__(S3Store)
    store.bucket, store.prefix = "bucket", "llm-cc"
    store._errors = (FakeClientError,)
    store.client = client
    return store


class ConditionalWriteTest(unittest.TestCase):
    KEY = "publications/marker.json"

    @unittest.skipUnless(os.name == "posix", "filesystem conditional writes use flock")
    def test_concurrent_filesystem_writers_never_lose_an_update(self):
        with tempfile.TemporaryDirectory() as directory:
            store = SlowFilesystemStore(directory)

            def increment():
                for _ in range(5):
                    while True:
                        raw, token = store.get_versioned(self.KEY)
                        value = 0 if raw is None else int(raw)
                        if store.put_if(self.KEY, b"%d" % (value + 1), token):
                            break

            threads = [threading.Thread(target=increment) for _ in range(6)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            self.assertEqual(store.get(self.KEY), b"30")

    @unittest.skipUnless(os.name == "posix", "filesystem conditional writes use flock")
    def test_absent_filesystem_key_is_created_exactly_once(self):
        with tempfile.TemporaryDirectory() as directory:
            store = SlowFilesystemStore(directory)
            barrier = threading.Barrier(6)
            tokens = []

            def create(index):
                barrier.wait()
                tokens.append(store.put_if(self.KEY, b"writer-%d" % index, None))

            threads = [
                threading.Thread(target=create, args=(index,)) for index in range(6)
            ]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            winners = [token for token in tokens if token is not None]
            self.assertEqual(len(winners), 1)
            value, token = store.get_versioned(self.KEY)
            self.assertEqual(token, winners[0])
            self.assertEqual(token, hashlib.sha256(value).hexdigest())
            # A stale token loses; the current one wins exactly once.
            self.assertIsNone(store.put_if(self.KEY, b"late", "0" * 64))
            self.assertIsNotNone(store.put_if(self.KEY, b"next", token))
            self.assertIsNone(store.put_if(self.KEY, b"again", token))
            with self.assertRaises(CacheError):
                store.put_if("../escape", b"x", None)

    def test_s3_conditional_writes_map_preconditions_to_conflicts(self):
        client = FakeS3Client()
        store = fake_s3_store(client)
        self.assertEqual(store.get_versioned(self.KEY), (None, None))
        first = store.put_if(self.KEY, b"one", None)
        self.assertEqual(client.requests[-1], {"IfMatch": None, "IfNoneMatch": "*"})
        self.assertIsNone(store.put_if(self.KEY, b"racing create", None))
        self.assertEqual(store.get_versioned(self.KEY), (b"one", first))
        second = store.put_if(self.KEY, b"two", first)
        self.assertEqual(client.requests[-1], {"IfMatch": first, "IfNoneMatch": None})
        self.assertIsNone(store.put_if(self.KEY, b"stale", first))
        del client.objects["llm-cc/" + self.KEY]
        self.assertIsNone(store.put_if(self.KEY, b"deleted meanwhile", second))

    def test_s3_access_failures_still_surface(self):
        client = FakeS3Client()

        def denied(**_):
            raise FakeClientError("AccessDenied")

        client.put_object = denied
        client.get_object = denied
        store = fake_s3_store(client)
        with self.assertRaises(CacheError):
            store.put_if(self.KEY, b"x", None)
        with self.assertRaises(CacheError):
            store.get_versioned(self.KEY)
