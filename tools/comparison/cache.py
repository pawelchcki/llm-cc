"""Content-addressed result storage.

The cache deliberately stores an envelope instead of a bare result.  That makes
an interrupted write, an old producer, or a manually edited entry a cache miss
rather than a way to silently contaminate a comparison.
"""

from __future__ import annotations

import hashlib
import json
import math
import os
import uuid
import base64
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


class CacheError(RuntimeError):
    """A store could not be read or written (as opposed to an invalid entry)."""


def _canonical(value: Any) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), allow_nan=False
    ).encode("utf-8")


def _now() -> datetime:
    return datetime.now(timezone.utc)


def _parse_time(value: str) -> datetime:
    if not isinstance(value, str):
        raise ValueError("cache timestamp must be a string")
    return datetime.fromisoformat(value.replace("Z", "+00:00"))


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
            raise CacheError("invalid cache key")
        return self.root.joinpath(*key.split("/"))

    def get(self, key: str) -> bytes | None:
        try:
            return self._path(key).read_bytes()
        except FileNotFoundError:
            return None
        except OSError as error:
            raise CacheError("cannot read cache entry %s: %s" % (key, error)) from error

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
            raise CacheError(
                "cannot write cache entry %s: %s" % (key, error)
            ) from error
        finally:
            if temporary is not None:
                try:
                    temporary.unlink(missing_ok=True)
                except OSError:
                    pass

    def list(self, prefix: str) -> list[str]:
        try:
            if not self.root.exists():
                return []

            def onerror(error):
                raise error

            names = []
            for directory, _, files in os.walk(self.root, onerror=onerror):
                for filename in files:
                    path = Path(directory) / filename
                    name = path.relative_to(self.root).as_posix()
                    if name.startswith(prefix) and not path.is_symlink():
                        names.append(name)
            return sorted(names)
        except OSError as error:
            raise CacheError("cannot list cache entries: %s" % error) from error


class S3Store:
    def __init__(self, bucket: str, prefix: str = "", **options: Any):
        try:
            import boto3  # type: ignore
            from botocore.config import Config
            from botocore.exceptions import ClientError, BotoCoreError  # type: ignore
        except ImportError as error:
            raise CacheError("S3 cache needs the optional boto3 dependency") from error
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
            raise CacheError("invalid cache key")
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
            raise CacheError(
                "cannot read S3 cache entry %s: %s" % (key, error)
            ) from error

    def put(self, key: str, value: bytes) -> None:
        try:
            self.client.put_object(Bucket=self.bucket, Key=self._key(key), Body=value)
        except self._errors as error:
            raise CacheError(
                "cannot write S3 cache entry %s: %s" % (key, error)
            ) from error

    def list(self, prefix: str) -> list[str]:
        try:
            token = None
            values: list[str] = []
            while True:
                args = {"Bucket": self.bucket, "Prefix": self._key(prefix)}
                if token:
                    args["ContinuationToken"] = token
                page = self.client.list_objects_v2(**args)
                for value in page.get("Contents", []):
                    name = value["Key"]
                    values.append(
                        name[len(self.prefix) :].lstrip("/") if self.prefix else name
                    )
                token = page.get("NextContinuationToken")
                if not page.get("IsTruncated"):
                    return sorted(values)
        except self._errors as error:
            raise CacheError("cannot list S3 cache entries: %s" % error) from error


def open_store(location: str, **options: Any) -> FilesystemStore | S3Store:
    if location.startswith("s3://"):
        rest = location[5:]
        bucket, _, prefix = rest.partition("/")
        if not bucket:
            raise CacheError("S3 cache location needs a bucket")
        return S3Store(bucket, prefix, **options)
    if "://" in location:
        raise CacheError("unsupported cache location: %s" % location)
    return FilesystemStore(location)


class ResultCache:
    def __init__(self, store: Any, refresh_days: int = 20, expire_days: int = 30):
        if refresh_days < 0 or expire_days < refresh_days:
            raise ValueError("cache retention must satisfy 0 <= refresh <= expire")
        self.store, self.refresh_days, self.expire_days = (
            store,
            refresh_days,
            expire_days,
        )

    @staticmethod
    def _key(item: dict[str, Any], fingerprint: str) -> str:
        key = item.get("key")
        expected = hashlib.sha256(
            _canonical([item.get("content_sha256"), item.get("language"), fingerprint])
        ).hexdigest()
        if key is not None and key != expected:
            raise ValueError("item key does not match its provenance")
        return "results/" + expected + ".json"

    def get(self, item: dict[str, Any], fingerprint: str) -> dict[str, Any] | None:
        try:
            cache_key = self._key(item, fingerprint)
        except (KeyError, TypeError, ValueError):
            return None
        raw = self.store.get(cache_key)
        if raw is None:
            return None
        try:
            envelope = json.loads(raw)
            result, digest = envelope["result"], envelope["sha256"]
            if (
                not isinstance(result, dict)
                or type(result.get("schema_version")) is not int
                or result["schema_version"] != 1
            ):
                return None
            if digest != hashlib.sha256(_canonical(result)).hexdigest():
                return None
            if (
                envelope["schema_version"] != 1
                or envelope["fingerprint"] != fingerprint
            ):
                return None
            if (
                result["content_sha256"] != item["content_sha256"]
                or result["language"] != item["language"]
            ):
                return None
            if result["fingerprint"] != fingerprint or result["key"] != item["key"]:
                return None
            when = _parse_time(envelope["stored_at"])
            age = (_now() - when).total_seconds() / 86400
            if age < 0 or age > self.expire_days:
                return None
            if (
                isinstance(result["token_count"], bool)
                or not isinstance(result["token_count"], int)
                or result["token_count"] < 0
            ):
                return None
            if (
                isinstance(result["llm_cc"], bool)
                or not isinstance(result["llm_cc"], (int, float))
                or not math.isfinite(result["llm_cc"])
                or result["llm_cc"] < 0
            ):
                return None
            # Refreshing a validated entry extends retention without changing
            # its provenance. A store failure is deliberately surfaced: it is
            # infrastructure failure, not a cache miss.
            if age > self.refresh_days:
                envelope["stored_at"] = _now().isoformat().replace("+00:00", "Z")
                self.store.put(cache_key, _canonical(envelope))
            return result
        except (
            KeyError,
            TypeError,
            ValueError,
            OverflowError,
            UnicodeDecodeError,
            json.JSONDecodeError,
        ):
            return None

    def put(self, result: dict[str, Any]) -> None:
        try:
            if (
                result.get("schema_version") != 1
                or not isinstance(result["key"], str)
                or not isinstance(result["content_sha256"], str)
                or not isinstance(result["language"], str)
                or not isinstance(result["fingerprint"], str)
                or isinstance(result["token_count"], bool)
                or not isinstance(result["token_count"], int)
                or result["token_count"] < 0
                or isinstance(result["llm_cc"], bool)
                or not isinstance(result["llm_cc"], (int, float))
                or not math.isfinite(result["llm_cc"])
                or result["llm_cc"] < 0
            ):
                raise ValueError("invalid cached result")
        except KeyError as error:
            raise ValueError("invalid cached result") from error
        item = {key: result[key] for key in ("key", "content_sha256", "language")}
        fingerprint = result["fingerprint"]
        key = self._key(item, fingerprint)
        envelope = {
            "schema_version": 1,
            "fingerprint": fingerprint,
            "stored_at": _now().isoformat().replace("+00:00", "Z"),
            "result": result,
            "sha256": hashlib.sha256(_canonical(result)).hexdigest(),
        }
        self.store.put(key, _canonical(envelope))

    # Native entropy is purposefully a different namespace and is never folded
    # into file results. Workers may safely retain individual entries.
    def get_entropy(self, key: str) -> bytes | None:
        return self.store.get("entropy/" + key)

    def put_entropy(self, key: str, value: bytes) -> None:
        self.store.put("entropy/" + key, value)

    def native_entropy(self, fingerprint: str) -> dict[str, bytes]:
        """Return individually checksummed native-cache files for a fingerprint.

        llm-cc independently checks its CBOR provenance when it consumes these;
        this layer verifies transport integrity and keeps model configurations
        isolated before they reach that parser.
        """
        output = {}
        for key in self.store.list("entropy/" + fingerprint + "/"):
            raw = self.store.get(key)
            if raw is None:
                continue
            try:
                envelope = json.loads(raw)
                name, payload = (
                    envelope["name"],
                    base64.b64decode(envelope["payload"], validate=True),
                )
                safe_name = Path(name)
                if (
                    envelope["schema_version"] != 1
                    or envelope["fingerprint"] != fingerprint
                    or safe_name.is_absolute()
                    or ".." in safe_name.parts
                    or not name.endswith(".cbor")
                    or key != "entropy/" + fingerprint + "/" + name
                    or envelope["sha256"] != hashlib.sha256(payload).hexdigest()
                ):
                    continue
                age = (
                    _now() - _parse_time(envelope["stored_at"])
                ).total_seconds() / 86400
                if age < 0 or age > self.expire_days:
                    continue
                output[name] = payload
            except (
                KeyError,
                TypeError,
                ValueError,
                UnicodeDecodeError,
                json.JSONDecodeError,
            ):
                continue
        return output

    def put_native_entropy(self, fingerprint: str, name: str, value: bytes) -> None:
        path = Path(name)
        if path.is_absolute() or ".." in path.parts or not name.endswith(".cbor"):
            raise ValueError("invalid native entropy entry name")
        envelope = {
            "schema_version": 1,
            "fingerprint": fingerprint,
            "name": name,
            "stored_at": _now().isoformat().replace("+00:00", "Z"),
            "payload": base64.b64encode(value).decode("ascii"),
            "sha256": hashlib.sha256(value).hexdigest(),
        }
        self.store.put("entropy/%s/%s" % (fingerprint, name), _canonical(envelope))
