import collections
import concurrent.futures
import hashlib
import itertools
import json
import math
import os
import re
from pathlib import Path

SCHEMA_VERSION = 1
CONTAINER_ENVIRONMENT_POLICY = "sanitized-v1"


def validate_execution_policy(profile):
    """Reject old container cache identities before any result can be reused."""
    build = profile["build"]
    if (
        build.get("execution_image") not in (None, "none")
        and build.get("container_environment_policy") != CONTAINER_ENVIRONMENT_POLICY
    ):
        raise ValueError(
            "container profile requires container_environment_policy="
            + CONTAINER_ENVIRONMENT_POLICY
            + "; regenerate the scoring profile to invalidate inherited-environment results"
        )


def canonical_bytes(value):
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=True, allow_nan=False
    ).encode("utf-8")


def digest(value):
    return hashlib.sha256(canonical_bytes(value)).hexdigest()


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp.%d" % os.getpid())
    temporary.write_bytes(canonical_bytes(value) + b"\n")
    os.replace(temporary, path)


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def valid_result(result, item, fingerprint):
    try:
        return (
            result["schema_version"] == 1
            and result["key"] == item["key"]
            and result["fingerprint"] == fingerprint
            and result["content_sha256"] == item["content_sha256"]
            and result["language"] == item["language"]
            and isinstance(result["token_count"], int)
            and not isinstance(result["token_count"], bool)
            and result["token_count"] >= 0
            and isinstance(result["llm_cc"], (int, float))
            and not isinstance(result["llm_cc"], bool)
            and math.isfinite(result["llm_cc"])
            and result["llm_cc"] >= 0
        )
    except (KeyError, TypeError):
        return False


# Mirrors src/model_identity.cc: `ModelFiles` discovers the companion shards of
# a split GGUF and `ModelDigest` domain-separates their digests, while the
# reported size is the shard total. A generated profile must pin the identity
# the scorer will report, or every comparison fails verification.
SPLIT_MODEL_DOMAIN = b"llm-cc-split-model-v1"
_SPLIT_MODEL_NAME = re.compile(r"(.*)-([0-9]{5})-of-([0-9]{5})\.gguf")


def model_files(path):
    """Every shard of the model `path` names, in scorer order.

    A name that does not match the split convention, or whose indices are
    inconsistent, describes itself alone -- exactly as the scorer decides it.
    """
    canonical = Path(os.path.realpath(path))
    match = _SPLIT_MODEL_NAME.fullmatch(canonical.name)
    if match is None:
        return [canonical]
    index, count = int(match.group(2)), int(match.group(3))
    if count <= 1 or index == 0 or index > count:
        return [canonical]
    shards = []
    for shard in range(1, count + 1):
        name = "%s-%05d-of-%05d.gguf" % (match.group(1), shard, count)
        companion = canonical.parent / name
        if not companion.is_file():
            raise ValueError("cannot resolve model shard " + name)
        shards.append(Path(os.path.realpath(companion)))
    return shards


def model_digest(digests):
    """The scorer's composite digest over already-hashed shards, in order."""
    if len(digests) == 1:
        return digests[0]
    composite = hashlib.sha256()
    composite.update(SPLIT_MODEL_DOMAIN)
    for value in digests:
        composite.update(b"\0")
        composite.update(value.encode("ascii"))
    return composite.hexdigest()


def bounded_map(function, arguments, concurrency):
    """Yield `function(argument)` in argument order with bounded concurrency.

    Only `concurrency` results are in flight or buffered at once, so a stage
    reading many large cache entries cannot be forced to hold all of them
    before the first is consumed. The first failure cancels queued work and
    propagates, so a broken store fails the run instead of quietly degrading
    into cache misses.
    """
    arguments = list(arguments)
    if concurrency == 1 or len(arguments) <= 1:
        for argument in arguments:
            yield function(argument)
        return
    executor = concurrent.futures.ThreadPoolExecutor(
        max_workers=concurrency, thread_name_prefix="bounded-map"
    )
    try:
        upcoming = iter(arguments)
        pending = collections.deque(
            executor.submit(function, argument)
            for argument in itertools.islice(upcoming, concurrency)
        )
        while pending:
            try:
                value = pending.popleft().result()
            except BaseException:
                executor.shutdown(wait=False, cancel_futures=True)
                raise
            for argument in itertools.islice(upcoming, 1):
                pending.append(executor.submit(function, argument))
            yield value
    finally:
        executor.shutdown(wait=True)
