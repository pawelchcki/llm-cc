import datetime
import json
import os
from pathlib import Path

from .cache import FilesystemStore
from .common import (
    SCHEMA_VERSION,
    aggregate_report,
    bounded_map,
    digest,
    valid_result,
    validate_execution_policy,
    write_json,
)
from .inventory import (
    changes,
    inventory,
    merge_base,
    read_tree_file,
    resolve_commit,
    validate_rules,
)

# `llm-cc compare aggregate` renders reports; comments stay within this.
COMMENT_LIMIT = 24 * 1024
# llm-cc reads the same file for local analysis; the legacy name still works.
REPOSITORY_RULES_PATH = (".llm-cc/rules.json", ".llm-cc/comparison-rules.json")


def _utc_now():
    return (
        datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00", "Z")
    )


def _reject_surrogates(value):
    if isinstance(value, dict):
        for key, item in value.items():
            _reject_surrogates(key)
            _reject_surrogates(item)
    elif isinstance(value, list):
        for item in value:
            _reject_surrogates(item)
    elif isinstance(value, str) and any(
        0xD800 <= ord(character) <= 0xDFFF for character in value
    ):
        raise ValueError("unpaired surrogate escape")


def resolve_rules(repo, target, rules, repository_rules_path):
    """Prefer the target commit's own rules; a PR cannot reclassify itself.

    `repository_rules_path` is one path or a sequence tried in order.
    """
    if isinstance(repository_rules_path, str):
        repository_rules_path = (repository_rules_path,)
    for path in repository_rules_path or ():
        raw = read_tree_file(repo, target, path)
        if raw is None:
            continue
        # llm-cc bounds the file itself, whitespace included.
        if len(raw) > 64 * 1024:
            raise ValueError(
                "repository classification rules at %s exceed 64 KiB" % path
            )
        try:
            # llm-cc's JSON parser accepts a leading byte order mark and
            # rejects lone surrogate escapes.
            candidate = json.loads(raw.decode("utf-8-sig"))
            _reject_surrogates(candidate)
        except (UnicodeError, ValueError) as error:
            raise ValueError(
                "repository classification rules at %s are not valid JSON: %s"
                % (path, error)
            ) from None
        return (
            validate_rules(candidate),
            {"source": "repository", "path": path, "commit": target},
        )
    return validate_rules(rules or {}), {"source": "host"}


def _lookup_results(cache, items, fingerprint, concurrency):
    """Read cached results with bounded concurrency, failing on the first error.

    Results are assembled from the sorted keys rather than completion order, so
    the plan is byte-identical at any concurrency.
    """
    keys = sorted(items)
    cached = {}
    if cache:
        # Authentication and transport errors must fail the run; queued reads
        # are pointless once one has failed.
        results = bounded_map(
            lambda key: cache.get(items[key], fingerprint), keys, concurrency
        )
        cached = dict(zip(keys, results))
    hits, misses = {}, []
    for key in keys:
        item = items[key]
        result = cached.get(key)
        if result is not None and valid_result(result, item, fingerprint):
            hits[key] = result
        else:
            misses.append(item)
    return hits, misses


def prepare(
    repo,
    head,
    target,
    identity,
    profile,
    rules,
    cache,
    output_dir,
    max_workers=4,
    repository_rules_path=REPOSITORY_RULES_PATH,
    presentation=None,
    cache_concurrency=8,
):
    validate_execution_policy(profile)
    if not 1 <= max_workers <= 4:
        raise ValueError("max_workers must be between 1 and 4")
    if type(cache_concurrency) is not int or not 1 <= cache_concurrency <= 64:
        raise ValueError("cache_concurrency must be between 1 and 64")
    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    fingerprint = digest({"scoring": profile["scoring"], "build": profile["build"]})
    head = resolve_commit(repo, head)
    target = resolve_commit(repo, target)
    base = merge_base(repo, target, head)
    full_identity = dict(identity)
    full_identity.update(head_sha=head, target_sha=target, base_sha=base)
    full_identity.setdefault("target_branch", None)
    full_identity.setdefault("pr_number", None)
    full_identity.setdefault("started_at", _utc_now())
    required = {
        "repository",
        "pipeline_id",
        "head_sha",
        "target_sha",
        "base_sha",
        "target_branch",
        "pr_number",
        "started_at",
    }
    if set(full_identity) != required:
        raise ValueError(
            "identity fields must be exactly: " + ", ".join(sorted(required))
        )
    limit = profile.get("max_file_bytes", 65536)
    if type(limit) is not int or limit <= 0:
        raise ValueError("max_file_bytes must be a positive integer")
    rules, rules_source = resolve_rules(repo, target, rules, repository_rules_path)
    base_inventory, base_blobs = inventory(repo, base, fingerprint, rules, limit)
    head_inventory, head_blobs = inventory(repo, head, fingerprint, rules, limit)
    all_blobs = base_blobs | head_blobs
    items = {}
    for record in base_inventory + head_inventory:
        if record["scorable"]:
            items.setdefault(
                record["key"],
                {
                    "key": record["key"],
                    "content_sha256": record["content_sha256"],
                    "language": record["language"],
                    "size": record["size"],
                    "blob": "blobs/" + record["content_sha256"],
                },
            )
    hits, misses = _lookup_results(cache, items, fingerprint, cache_concurrency)
    blob_store = FilesystemStore(output)
    for item in misses:
        # A previous preparation may have stopped halfway through a write.
        # Re-publish authoritative Git bytes atomically before exposing the plan.
        blob_store.put(item["blob"], all_blobs[item["content_sha256"]])
    workers = []
    if misses:
        workers = [
            {"worker_id": i, "keys": [], "bytes": 0}
            for i in range(min(max_workers, len(misses)))
        ]
        for item in sorted(misses, key=lambda x: (-x["size"], x["key"])):
            worker = min(workers, key=lambda x: (x["bytes"], x["worker_id"]))
            worker["keys"].append(item["key"])
            worker["bytes"] += item["size"]
    plan = {
        "schema_version": SCHEMA_VERSION,
        "identity": full_identity,
        "fingerprint": fingerprint,
        "profile": profile,
        "inventories": {"base": base_inventory, "head": head_inventory},
        "changes": changes(repo, base, head),
        "rules": rules,
        "rules_source": rules_source,
        "presentation": presentation or {},
        "items": items,
        "hits": hits,
        "workers": workers,
        "cache_stats": {
            "items": len(items),
            "hits": len(hits),
            "misses": len(misses),
            "hit_bytes": sum(items[k]["size"] for k in hits),
            "miss_bytes": sum(x["size"] for x in misses),
        },
    }
    write_json(output / "plan.json", plan)
    return plan


def compare(
    repo,
    head,
    target,
    identity,
    profile,
    rules,
    cache,
    output_dir,
    scorer,
    model,
    installed_root,
    max_workers=4,
    deadline_seconds=6600,
    repository_rules_path=REPOSITORY_RULES_PATH,
    presentation=None,
    cache_concurrency=8,
):
    from .worker import run_worker

    if isinstance(scorer, os.PathLike):
        scorer = str(scorer)
    plan = prepare(
        repo,
        head,
        target,
        identity,
        profile,
        rules,
        cache,
        output_dir,
        max_workers,
        repository_rules_path,
        presentation,
        cache_concurrency,
    )
    if plan["workers"] and not all((scorer, model, installed_root)):
        # Keep the plan's fingerprint and partial analysis with the failure.
        return aggregate_report(
            output_dir,
            plan=Path(output_dir) / "plan.json",
            identity=plan["identity"],
            errors=["cache misses require --scorer, --model, and --installed-root"],
        )
    worker_paths = []
    for worker in plan["workers"]:
        run_worker(
            Path(output_dir) / "plan.json",
            worker["worker_id"],
            cache.store,
            output_dir,
            scorer,
            model,
            installed_root,
            deadline_seconds,
            cache_concurrency,
        )
        worker_paths.append(Path(output_dir) / ("worker-%d.json" % worker["worker_id"]))
    return aggregate_report(
        output_dir, plan=Path(output_dir) / "plan.json", workers=worker_paths
    )
