"""Whole-object stores for pipeline transport, reports and publication markers.

`llm-cc compare` reads and writes the result and entropy caches itself; these
stores only move plans, blobs, worker artifacts and reports, and order
publication with conditional writes.
"""

from __future__ import annotations

import hashlib
import os
import uuid
from pathlib import Path
from typing import Any


class StoreError(RuntimeError):
    """A store could not be read or written."""


class FilesystemStore:
    """A small atomic object store rooted at a directory."""

    def __init__(self, root: str | os.PathLike[str]):
        self.root = Path(root)

    def _path(self, key: str) -> Path:
        if (
            not key
            or key.startswith("/")
            or any(part in ("", ".", "..") for part in key.split("/"))
        ):
            raise StoreError("invalid store key")
        return self.root.joinpath(*key.split("/"))

    def get(self, key: str) -> bytes | None:
        try:
            return self._path(key).read_bytes()
        except FileNotFoundError:
            return None
        except OSError as error:
            raise StoreError(
                "cannot read store object %s: %s" % (key, error)
            ) from error

    def put(self, key: str, value: bytes) -> None:
        destination = self._path(key)
        temporary = None
        try:
            destination.parent.mkdir(parents=True, exist_ok=True)
            temporary = destination.with_name(
                ".%s.%d.%s.tmp" % (destination.name, os.getpid(), uuid.uuid4().hex)
            )
            # O_EXCL avoids two writers mistaking the same temporary file for
            # their own. replace is atomic on the same filesystem.
            # Honor the deployment's umask/default ACL so a provisioned shared
            # group can read objects published by another executor account.
            fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o660)
            with os.fdopen(fd, "wb") as stream:
                stream.write(value)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, destination)
            try:
                directory_fd = os.open(destination.parent, os.O_RDONLY)
                try:
                    os.fsync(directory_fd)
                finally:
                    os.close(directory_fd)
            except OSError:  # directory fsync is unavailable on some platforms
                pass
        except OSError as error:
            raise StoreError(
                "cannot write store object %s: %s" % (key, error)
            ) from error
        finally:
            if temporary is not None:
                try:
                    temporary.unlink(missing_ok=True)
                except OSError:
                    pass

    def get_versioned(self, key: str) -> tuple[bytes | None, str | None]:
        """Read an object with a version token for `put_if`: its SHA-256."""
        value = self.get(key)
        return value, None if value is None else hashlib.sha256(value).hexdigest()

    def put_if(self, key: str, value: bytes, expected: str | None) -> str | None:
        """Replace `key` only while it is still at version `expected`.

        `expected=None` creates an absent object. Returns the new version
        token, or None when another writer changed the object first. Writers
        serialize on a per-key `flock`, so the store must be local or on a
        filesystem with working advisory locks; readers stay lock-free because
        the replacement itself is atomic.
        """
        try:
            import fcntl
        except ImportError as error:
            raise StoreError(
                "conditional filesystem writes need POSIX flock"
            ) from error
        self._path(key)
        lock = (
            self.root / ".locks" / (hashlib.sha256(key.encode()).hexdigest() + ".lock")
        )
        try:
            lock.parent.mkdir(parents=True, exist_ok=True)
            fd = os.open(lock, os.O_RDWR | os.O_CREAT, 0o660)
        except OSError as error:
            raise StoreError(
                "cannot lock store object %s: %s" % (key, error)
            ) from error
        try:
            fcntl.flock(fd, fcntl.LOCK_EX)
            if self.get_versioned(key)[1] != expected:
                return None
            self.put(key, value)
            return hashlib.sha256(value).hexdigest()
        finally:
            os.close(fd)


class S3Store:
    def __init__(self, bucket: str, prefix: str = "", **options: Any):
        try:
            import boto3  # type: ignore
            from botocore.config import Config
            from botocore.exceptions import ClientError, BotoCoreError  # type: ignore
        except ImportError as error:
            raise StoreError("S3 stores need the optional boto3 dependency") from error
        self.bucket, self.prefix = bucket, prefix.strip("/")
        self._errors = (ClientError, BotoCoreError, OSError)
        kwargs = {key: value for key, value in options.items() if value is not None}
        kwargs.setdefault(
            "config",
            Config(connect_timeout=10, read_timeout=30, retries={"max_attempts": 2}),
        )
        self.client = boto3.client("s3", **kwargs)

    def _key(self, key: str) -> str:
        if not key or key.startswith("/") or ".." in key.split("/"):
            raise StoreError("invalid store key")
        return "/".join(part for part in (self.prefix, key) if part)

    def get(self, key: str) -> bytes | None:
        try:
            return self.client.get_object(Bucket=self.bucket, Key=self._key(key))[
                "Body"
            ].read()
        except self._errors as error:
            code = getattr(error, "response", {}).get("Error", {}).get("Code")
            if code in ("NoSuchKey", "404", "NotFound"):
                return None
            raise StoreError("cannot read S3 object %s: %s" % (key, error)) from error

    def put(self, key: str, value: bytes) -> None:
        try:
            self.client.put_object(Bucket=self.bucket, Key=self._key(key), Body=value)
        except self._errors as error:
            raise StoreError("cannot write S3 object %s: %s" % (key, error)) from error

    def get_versioned(self, key: str) -> tuple[bytes | None, str | None]:
        """Read an object with a version token for `put_if`: its ETag."""
        try:
            response = self.client.get_object(Bucket=self.bucket, Key=self._key(key))
            return response["Body"].read(), response["ETag"]
        except self._errors as error:
            code = getattr(error, "response", {}).get("Error", {}).get("Code")
            if code in ("NoSuchKey", "404", "NotFound"):
                return None, None
            raise StoreError("cannot read S3 object %s: %s" % (key, error)) from error

    def put_if(self, key: str, value: bytes, expected: str | None) -> str | None:
        """Conditional PutObject: If-None-Match to create, If-Match to replace.

        Returns the new ETag, or None when the object changed first. Stores
        without conditional writes fail loudly instead of silently racing.
        """
        condition = {"IfNoneMatch": "*"} if expected is None else {"IfMatch": expected}
        try:
            return self.client.put_object(
                Bucket=self.bucket, Key=self._key(key), Body=value, **condition
            )["ETag"]
        except self._errors as error:
            code = getattr(error, "response", {}).get("Error", {}).get("Code")
            # NoSuchKey answers If-Match when the object was deleted meanwhile.
            if code in (
                "PreconditionFailed",
                "ConditionalRequestConflict",
                "NoSuchKey",
            ):
                return None
            raise StoreError(
                "cannot conditionally write S3 object %s: %s" % (key, error)
            ) from error


def open_store(location: str, **options: Any) -> FilesystemStore | S3Store:
    if location.startswith("s3://"):
        rest = location[5:]
        bucket, _, prefix = rest.partition("/")
        if not bucket:
            raise StoreError("S3 store location needs a bucket")
        return S3Store(bucket, prefix, **options)
    if "://" in location:
        raise StoreError("unsupported store location: %s" % location)
    return FilesystemStore(location)
