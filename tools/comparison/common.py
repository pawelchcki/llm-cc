import hashlib
import json
import math
import os
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
