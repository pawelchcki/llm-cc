import hashlib
import io
import os
import stat
import tempfile
import threading
import time
import unittest
from pathlib import Path

from tools.comparison.store import FilesystemStore, S3Store, StoreError, open_store


class FilesystemStoreTest(unittest.TestCase):
    def test_missing_objects_read_as_none_and_keys_stay_inside(self):
        with tempfile.TemporaryDirectory() as directory:
            store = FilesystemStore(directory)
            self.assertIsNone(store.get("pipelines/absent.json"))
            for key in ("", "/absolute", "a/../b", "a//b", "./a"):
                with self.subTest(key=key), self.assertRaises(StoreError):
                    store.put(key, b"x")

    def test_concurrent_atomic_writes_leave_a_complete_object(self):
        with tempfile.TemporaryDirectory() as directory:
            store = FilesystemStore(directory)
            values = [bytes([65 + index]) * 4096 for index in range(12)]
            threads = [
                threading.Thread(target=store.put, args=("reports/r.json", value))
                for value in values
            ]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            self.assertIn(store.get("reports/r.json"), values)
            leftovers = [
                path.name
                for path in Path(directory, "reports").iterdir()
                if path.name != "r.json"
            ]
            self.assertEqual(leftovers, [])

    @unittest.skipUnless(os.name == "posix", "POSIX shared-store permissions")
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

    @unittest.skipUnless(os.name == "posix", "POSIX symlinks")
    def test_objects_never_resolve_through_a_symlink(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory, "store")
            outside = Path(directory, "outside")
            outside.mkdir()
            (outside / "x.json").write_bytes(b"outside")
            store = FilesystemStore(root)
            store.put("reports/r.json", b"inside")
            (root / "linked").symlink_to(outside)
            (root / "alias.json").symlink_to(root / "reports/r.json")
            for key in ("linked/x.json", "alias.json"):
                with self.subTest(key=key), self.assertRaises(StoreError):
                    store.get(key)
            for key in ("linked/y.json", "alias.json"):
                with self.subTest(key=key), self.assertRaises(StoreError):
                    store.put(key, b"escaped")
            self.assertFalse((outside / "y.json").exists())
            self.assertEqual(store.get("reports/r.json"), b"inside")

    def test_locations_select_a_store(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertIsInstance(open_store(directory), FilesystemStore)
        for location in ("s3://", "gs://bucket/prefix"):
            with self.subTest(location=location), self.assertRaises(StoreError):
                open_store(location)


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
            with self.assertRaises(StoreError):
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
        with self.assertRaises(StoreError):
            store.put_if(self.KEY, b"x", None)
        with self.assertRaises(StoreError):
            store.get_versioned(self.KEY)

    def test_s3_missing_objects_read_as_none(self):
        store = fake_s3_store(FakeS3Client())
        self.assertIsNone(store.get(self.KEY))
        store.put(self.KEY, b"stored")
        self.assertEqual(store.get(self.KEY), b"stored")
