"""Whole-object stores for pipeline transport, reports and publication markers.

`llm-cc compare` reads and writes the result and entropy caches itself; these
stores only move plans, blobs, worker artifacts and reports, and order
publication with conditional writes.
"""

from __future__ import annotations

import hashlib
import os
import stat
import uuid
from pathlib import Path
from typing import Any


class StoreError(RuntimeError):
    """A store could not be read or written."""


# Descriptor-relative traversal needs POSIX *at() calls; elsewhere (Windows,
# where creating symlinks needs privilege) objects are addressed by path.
_DESCRIPTORS = (
    os.open in os.supports_dir_fd
    and os.mkdir in os.supports_dir_fd
    and hasattr(os, "O_NOFOLLOW")
    and hasattr(os, "O_DIRECTORY")
)


class FilesystemStore:
    """A small atomic object store rooted at a directory."""

    def __init__(self, root: str | os.PathLike[str]):
        self.root = Path(root)

    def _parts(self, key: str) -> list[str]:
        parts = key.split("/")
        # A backslash is a separator and a colon a drive or stream on
        # Windows, as llm-cc's store key validation also refuses.
        if (
            not key
            or key.startswith("/")
            or any(p in ("", ".", "..") or "\\" in p or ":" in p for p in parts)
        ):
            raise StoreError("invalid store key")
        return parts

    def _path(self, key: str) -> Path:
        return self.root.joinpath(*self._parts(key))

    def _parent(self, key: str, create: bool) -> tuple[int | None, str]:
        """The directory holding `key`, opened without following symlinks.

        Another writer of a shared store could swap a key directory for a
        symlink, so each directory is opened relative to the previous one with
        O_NOFOLLOW, and the object is then read or replaced through the last
        descriptor. With `create`, missing directories are made one level at
        a time; otherwise a missing one returns no descriptor, a miss.
        """
        parts = self._parts(key)
        flags = os.O_RDONLY | os.O_DIRECTORY
        if create:
            self.root.mkdir(parents=True, exist_ok=True)
        try:
            descriptor = os.open(self.root, flags)
        except FileNotFoundError:
            return None, parts[-1]
        try:
            for part in parts[:-1]:
                try:
                    child = os.open(part, flags | os.O_NOFOLLOW, dir_fd=descriptor)
                except FileNotFoundError:
                    if not create:
                        os.close(descriptor)
                        return None, parts[-1]
                    try:
                        os.mkdir(part, 0o777, dir_fd=descriptor)
                    except FileExistsError:
                        pass
                    child = os.open(part, flags | os.O_NOFOLLOW, dir_fd=descriptor)
                os.close(descriptor)
                descriptor = child
        except OSError as error:
            os.close(descriptor)
            raise StoreError(
                "store object %s passes through a symlink or something other "
                "than a directory: %s" % (key, error)
            ) from error
        return descriptor, parts[-1]

    def get(self, key: str) -> bytes | None:
        if not _DESCRIPTORS:
            return self._get_by_path(key)
        directory, name = self._parent(key, create=False)
        if directory is None:
            return None
        try:
            # O_NONBLOCK keeps a planted FIFO from blocking the open.
            descriptor = os.open(
                name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory
            )
        except FileNotFoundError:
            return None
        except OSError as error:
            raise StoreError(
                "cannot read store object %s: %s" % (key, error)
            ) from error
        finally:
            os.close(directory)
        try:
            if not stat.S_ISREG(os.fstat(descriptor).st_mode):
                raise StoreError("cannot read store object %s: not a file" % key)
            with os.fdopen(descriptor, "rb") as stream:
                descriptor = None
                return stream.read()
        except OSError as error:
            raise StoreError(
                "cannot read store object %s: %s" % (key, error)
            ) from error
        finally:
            if descriptor is not None:
                os.close(descriptor)

    def put(self, key: str, value: bytes) -> None:
        if not _DESCRIPTORS:
            return self._put_by_path(key, value)
        directory, name = self._parent(key, create=True)
        temporary = ".%s.%d.%s.tmp" % (name, os.getpid(), uuid.uuid4().hex)
        created = False
        try:
            try:
                existing = os.stat(name, dir_fd=directory, follow_symlinks=False)
                if stat.S_ISLNK(existing.st_mode):
                    raise StoreError("store object %s is a symlink" % key)
            except FileNotFoundError:
                pass
            # O_EXCL avoids two writers mistaking the same temporary file for
            # their own. Honor the deployment's umask/default ACL so a
            # provisioned shared group can read objects another account wrote.
            descriptor = os.open(
                temporary,
                os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                0o660,
                dir_fd=directory,
            )
            created = True
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(value)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, name, src_dir_fd=directory, dst_dir_fd=directory)
            created = False
            os.fsync(directory)
        except OSError as error:
            raise StoreError(
                "cannot write store object %s: %s" % (key, error)
            ) from error
        finally:
            if created:
                try:
                    os.unlink(temporary, dir_fd=directory)
                except OSError:
                    pass
            os.close(directory)

    def _get_by_path(self, key: str) -> bytes | None:
        try:
            return self._path(key).read_bytes()
        except FileNotFoundError:
            return None
        except OSError as error:
            raise StoreError(
                "cannot read store object %s: %s" % (key, error)
            ) from error

    def _put_by_path(self, key: str, value: bytes) -> None:
        destination = self._path(key)
        temporary = None
        try:
            destination.parent.mkdir(parents=True, exist_ok=True)
            temporary = destination.with_name(
                ".%s.%d.%s.tmp" % (destination.name, os.getpid(), uuid.uuid4().hex)
            )
            fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o660)
            with os.fdopen(fd, "wb") as stream:
                stream.write(value)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, destination)
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
