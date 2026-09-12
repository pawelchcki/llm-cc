import datetime
import hashlib
import os
import unicodedata
from pathlib import Path

from .common import SCHEMA_VERSION, digest, read_json, valid_result, write_json
from .inventory import changes, inventory, merge_base, resolve_commit


def _utc_now():
    return (
        datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00", "Z")
    )


def prepare(
    repo, head, target, identity, profile, rules, cache, output_dir, max_workers=4
):
    if not 1 <= max_workers <= 4:
        raise ValueError("max_workers must be between 1 and 4")
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
    hits = {}
    misses = []
    for key, item in sorted(items.items()):
        cached = cache.get(item, fingerprint) if cache else None
        if cached is not None and valid_result(cached, item, fingerprint):
            hits[key] = cached
        else:
            misses.append(item)
    for item in misses:
        blob_path = output / item["blob"]
        blob_path.parent.mkdir(parents=True, exist_ok=True)
        if not blob_path.exists():
            blob_path.write_bytes(all_blobs[item["content_sha256"]])
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


def _side(inventory, results):
    categories = {
        name: {
            "paths": 0,
            "supported_paths": 0,
            "measured_paths": 0,
            "bytes": 0,
            "supported_bytes": 0,
            "measured_bytes": 0,
            "measured_llm_cc": 0.0,
            "measured_token_count": 0,
        }
        for name in ("runtime", "tests", "tooling")
    }
    reasons = {}
    supported = 0
    measured = 0
    supported_bytes = 0
    measured_bytes = 0
    for file in inventory:
        cat = categories[file["category"]]
        cat["paths"] += 1
        cat["bytes"] += file["size"] or 0
        supported_file = file["scorable"] or (
            file.get("language") is not None and file.get("reason") == "oversized"
        )
        if supported_file:
            supported += 1
            cat["supported_paths"] += 1
            supported_bytes += file["size"] or 0
            cat["supported_bytes"] += file["size"] or 0
        if file["scorable"]:
            result = results.get(file["key"])
            if result:
                measured += 1
                measured_bytes += file["size"]
                cat["measured_paths"] += 1
                cat["measured_bytes"] += file["size"]
                cat["measured_llm_cc"] += result["llm_cc"]
                cat["measured_token_count"] += result["token_count"]
        else:
            reasons[file["reason"]] = reasons.get(file["reason"], 0) + 1
    for cat in categories.values():
        complete = cat["measured_paths"] == cat["supported_paths"]
        cat["llm_cc"] = cat["measured_llm_cc"] if complete else None
        cat["token_count"] = cat["measured_token_count"] if complete else None
        cat["score"] = (
            cat["llm_cc"] / cat["token_count"]
            if complete and cat["token_count"]
            else None
        )
        cat["coverage"] = (
            cat["measured_paths"] / cat["supported_paths"]
            if cat["supported_paths"]
            else 1.0
        )
        cat["byte_coverage"] = (
            cat["measured_bytes"] / cat["supported_bytes"]
            if cat["supported_bytes"]
            else 1.0
        )
    measured_llm_cc = sum(x["measured_llm_cc"] for x in categories.values())
    measured_tokens = sum(x["measured_token_count"] for x in categories.values())
    complete = measured == supported
    totals = {
        "paths": len(inventory),
        "supported_paths": supported,
        "measured_paths": measured,
        "coverage": measured / supported if supported else 1.0,
        "unmeasured_reasons": reasons,
        "supported_bytes": supported_bytes,
        "measured_bytes": measured_bytes,
        "byte_coverage": measured_bytes / supported_bytes if supported_bytes else 1.0,
        "measured_llm_cc": measured_llm_cc,
        "measured_token_count": measured_tokens,
        "llm_cc": measured_llm_cc if complete else None,
        "token_count": measured_tokens if complete else None,
    }
    totals["score"] = (
        totals["llm_cc"] / totals["token_count"]
        if complete and totals["token_count"]
        else None
    )
    return {"totals": totals, "categories": categories}


def _validate_plan(plan):
    if plan.get("schema_version") != SCHEMA_VERSION:
        raise ValueError("unsupported plan schema version")
    expected_fingerprint = digest(
        {"scoring": plan["profile"]["scoring"], "build": plan["profile"]["build"]}
    )
    if plan.get("fingerprint") != expected_fingerprint:
        raise ValueError("plan fingerprint does not match its profile")
    items = plan["items"]
    for key, item in items.items():
        expected_key = digest(
            [item["content_sha256"], item["language"], expected_fingerprint]
        )
        if key != expected_key or item.get("key") != key:
            raise ValueError("plan item key does not match its provenance")
    referenced = set()
    for side in ("base", "head"):
        paths = set()
        for file in plan["inventories"][side]:
            if file["path"] in paths:
                raise ValueError("inventory has duplicate paths")
            paths.add(file["path"])
            if file["category"] not in {"runtime", "tests", "tooling"}:
                raise ValueError("inventory has an invalid category")
            if file["scorable"] and (
                file.get("key") not in items or file.get("reason") is not None
            ):
                raise ValueError("inventory references an invalid scorable item")
            if not file["scorable"] and file.get("key") is not None:
                raise ValueError("unscorable inventory entry has an item key")
            if file["scorable"]:
                item = items[file["key"]]
                if any(
                    file[field] != item[field]
                    for field in ("content_sha256", "language", "size")
                ):
                    raise ValueError("inventory provenance differs from scoring item")
                referenced.add(file["key"])
    if referenced != set(items):
        raise ValueError("plan items do not exactly match the inventories")
    hits = plan.get("hits", {})
    for key, result in hits.items():
        if key not in items or not valid_result(
            result, items[key], expected_fingerprint
        ):
            raise ValueError("plan contains an invalid cached result")
    assigned = []
    worker_ids = []
    for worker in plan["workers"]:
        if (
            type(worker["worker_id"]) is not int
            or not 0 <= worker["worker_id"] < 4
            or not worker["keys"]
        ):
            raise ValueError("invalid worker assignment")
        worker_ids.append(worker["worker_id"])
        assigned.extend(worker["keys"])
    if len(worker_ids) != len(set(worker_ids)) or len(assigned) != len(set(assigned)):
        raise ValueError("plan has duplicate workers or assignments")
    if set(assigned) != set(items) - set(hits):
        raise ValueError("worker assignments do not exactly cover cache misses")


def _delta(base, head):
    absolute = head - base if base is not None and head is not None else None
    percent = absolute / base * 100 if absolute is not None and base != 0 else None
    return {"base": base, "head": head, "absolute": absolute, "percent": percent}


def _safe(text):
    value = str(text).encode("utf-8", "backslashreplace").decode("utf-8")
    value = "".join(
        character
        if unicodedata.category(character) not in {"Cc", "Cf"}
        and character not in "\u2028\u2029"
        else "\\u%04x" % ord(character)
        for character in value
    )
    return (
        value.replace("&", "&amp;")
        .replace("#", "&#35;")
        .replace("@", "&#64;&#8203;")
        .replace(".", "&#46;")
        .replace("/", "&#47;")
        .replace(":", "&#58;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace("`", "&#96;")
        .replace("|", "&#124;")
        .replace("[", "&#91;")
        .replace("]", "&#93;")
        .replace("*", "&#42;")
        .replace("_", "&#95;")
        .replace("~", "&#126;")
        .replace("$", "&#36;")
        .replace("+", "&#43;")
        .replace("-", "&#45;")
        .replace("=", "&#61;")
        .replace("!", "&#33;")
        .replace("(", "&#40;")
        .replace(")", "&#41;")
        .replace("\\", "&#92;")
    )


def _render(report, full=False):
    lines = [
        "## llm-cc comparison",
        "",
        "Status: **%s**" % report["status"],
        "",
        "| Category | Score base | Score head | Score Δ | Score Δ% | Raw LLM Δ | Tokens Δ | Coverage |",
        "|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for name in ("runtime", "tests", "tooling", "repository"):
        item = report["comparisons"][name]
        base = item["score"]["base"]
        head = item["score"]["head"]
        change = item["score"]["absolute"]

        def fmt(x):
            return "unavailable" if x is None else "%.6g" % x

        lines.append(
            "| %s | %s | %s | %s | %s | %s | %s | %.1f%% / %.1f%% |"
            % (
                _safe(name),
                fmt(base),
                fmt(head),
                fmt(change),
                "unavailable"
                if item["score"]["percent"] is None
                else "%+.3g%%" % item["score"]["percent"],
                fmt(item["raw_llm_cc"]["absolute"]),
                fmt(item["tokens"]["absolute"]),
                item["coverage"]["base"] * 100,
                item["coverage"]["head"] * 100,
            )
        )
    lines += [
        "",
        "Cache: %d hits, %d misses."
        % (report["cache_stats"]["hits"], report["cache_stats"]["misses"]),
    ]
    if report["leading_regressions"]:
        lines += ["", "### Leading regressions", ""] + [
            "- %s: %+.6g" % (_safe(x["path"]), x["change"])
            for x in report["leading_regressions"]
        ]
    if report["leading_improvements"]:
        lines += ["", "### Leading improvements", ""] + [
            "- %s: %+.6g" % (_safe(x["path"]), x["change"])
            for x in report["leading_improvements"]
        ]
    if report["errors"]:
        lines += ["", "Errors:"] + [
            "- " + _safe(error) for error in report["errors"][:20]
        ]
    if full:
        lines += [
            "",
            "### Raw totals",
            "",
            "| Side/category | LLM total | Tokens | Score |",
            "|---|---:|---:|---:|",
        ]
        for side in ("base", "head"):
            for name in ("runtime", "tests", "tooling"):
                values = report["sides"][side]["categories"][name]
                lines.append(
                    "| %s/%s | %s | %s | %s |"
                    % (
                        side,
                        name,
                        fmt(values["llm_cc"]),
                        fmt(values["token_count"]),
                        fmt(values["score"]),
                    )
                )
        lines += ["", "### Coverage details", ""]
        for side in ("base", "head"):
            totals = report["sides"][side]["totals"]
            reasons = (
                ", ".join(
                    "%s: %d" % (_safe(k), v)
                    for k, v in sorted(totals["unmeasured_reasons"].items())
                )
                or "none"
            )
            lines.append(
                "- %s: %d/%d supported paths measured; unscored paths: %s"
                % (
                    side.title(),
                    totals["measured_paths"],
                    totals["supported_paths"],
                    reasons,
                )
            )
        lines += ["", "### Changed paths", ""]
        if report["changes"]:
            for change in report["changes"]:
                old = (
                    _safe(change["old_path"]) if change["old_path"] is not None else "∅"
                )
                new = (
                    _safe(change["new_path"]) if change["new_path"] is not None else "∅"
                )
                lines.append("- %s → %s (%s)" % (old, new, _safe(change["status"])))
        else:
            lines.append("- No changed paths.")
        for side in ("base", "head"):
            lines += [
                "",
                "### %s inventory" % side.title(),
                "",
                "| Path | Category | Language | Bytes | Measurement |",
                "|---|---|---|---:|---|",
            ]
            for file in report["inventories"][side]:
                measurement = (
                    "measured"
                    if file["scorable"] and file["key"] in report["results"]
                    else (file["reason"] or "missing")
                )
                lines.append(
                    "| %s | %s | %s | %s | %s |"
                    % (
                        _safe(file["path"]),
                        _safe(file["category"]),
                        _safe(file["language"] or "—"),
                        file["size"] if file["size"] is not None else "—",
                        _safe(measurement),
                    )
                )
    return "\n".join(lines) + "\n"


def _aggregate(plan, worker_paths, output):
    _validate_plan(plan)
    errors = []
    results = dict(plan.get("hits", {}))
    expected_workers = {w["worker_id"]: set(w["keys"]) for w in plan["workers"]}
    seen_workers = set()
    for path in worker_paths:
        try:
            artifact = read_json(path)
        except Exception as exc:
            errors.append("cannot read worker artifact %s: %s" % (path, exc))
            continue
        if (
            not isinstance(artifact, dict)
            or type(artifact.get("worker_id")) is not int
            or not isinstance(artifact.get("results"), dict)
            or not isinstance(artifact.get("errors"), list)
            or any(not isinstance(error, str) for error in artifact["errors"])
        ):
            errors.append("malformed worker artifact: %s" % path)
            continue
        worker_id = artifact.get("worker_id")
        if (
            artifact.get("identity") != plan["identity"]
            or artifact.get("fingerprint") != plan["fingerprint"]
        ):
            errors.append(
                "worker %s belongs to another pipeline or fingerprint" % worker_id
            )
            continue
        if worker_id not in expected_workers or worker_id in seen_workers:
            errors.append("unexpected or duplicate worker %s" % worker_id)
            continue
        seen_workers.add(worker_id)
        actual = set(artifact.get("results", {}))
        expected = expected_workers[worker_id]
        artifact_errors = []
        if artifact.get("schema_version") != SCHEMA_VERSION:
            artifact_errors.append("unsupported schema")
        if actual != expected:
            artifact_errors.append(
                "result coverage mismatch (missing %d, extra %d)"
                % (len(expected - actual), len(actual - expected))
            )
        if artifact.get("status") != "complete":
            artifact_errors.append("failed: %s" % "; ".join(artifact.get("errors", [])))
        elif artifact["errors"]:
            artifact_errors.append("complete worker reported errors")
        for key, result in artifact.get("results", {}).items():
            item = plan["items"].get(key)
            if (
                key in expected
                and item
                and not valid_result(result, item, plan["fingerprint"])
            ):
                artifact_errors.append("invalid result %s" % key)
        if artifact_errors:
            errors.extend(
                "worker %s %s" % (worker_id, error) for error in artifact_errors
            )
            continue
        results.update(artifact["results"])
    missing_workers = set(expected_workers) - seen_workers
    if missing_workers:
        errors.append(
            "missing worker artifacts: " + ", ".join(map(str, sorted(missing_workers)))
        )
    expected_keys = set(plan["items"])
    missing = expected_keys - set(results)
    if missing:
        errors.append("unmeasured unique files: %d" % len(missing))
    base = _side(plan["inventories"]["base"], results)
    head = _side(plan["inventories"]["head"], results)
    comparisons = {}
    for name in ("runtime", "tests", "tooling"):
        b, h = base["categories"][name], head["categories"][name]
        comparisons[name] = {
            "score": _delta(b["score"], h["score"]),
            "raw_llm_cc": _delta(b["llm_cc"], h["llm_cc"]),
            "tokens": _delta(b["token_count"], h["token_count"]),
            "coverage": {
                "base": b["measured_paths"] / b["supported_paths"]
                if b["supported_paths"]
                else 1.0,
                "head": h["measured_paths"] / h["supported_paths"]
                if h["supported_paths"]
                else 1.0,
            },
        }
    comparisons["repository"] = {
        "score": _delta(base["totals"]["score"], head["totals"]["score"]),
        "raw_llm_cc": _delta(base["totals"]["llm_cc"], head["totals"]["llm_cc"]),
        "tokens": _delta(base["totals"]["token_count"], head["totals"]["token_count"]),
        "coverage": {
            "base": base["totals"]["coverage"],
            "head": head["totals"]["coverage"],
        },
    }
    counts = {"additions": 0, "deletions": 0, "renames": 0, "category_moves": 0}
    base_paths = {x["path"]: x for x in plan["inventories"]["base"]}
    head_paths = {x["path"]: x for x in plan["inventories"]["head"]}
    for change in plan["changes"]:
        status = change["status"]
        if status.startswith("A"):
            counts["additions"] += 1
        elif status.startswith("D"):
            counts["deletions"] += 1
        elif status.startswith("R"):
            counts["renames"] += 1
        old = base_paths.get(change["old_path"])
        new = head_paths.get(change["new_path"])
        if old and new and old["category"] != new["category"]:
            counts["category_moves"] += 1
    path_deltas = []
    for change in plan["changes"]:
        old = base_paths.get(change["old_path"])
        new = head_paths.get(change["new_path"])
        old_result = results.get(old["key"]) if old and old["key"] else None
        new_result = results.get(new["key"]) if new and new["key"] else None
        if (
            old_result
            and new_result
            and old_result["token_count"]
            and new_result["token_count"]
        ):
            value = (
                new_result["llm_cc"] / new_result["token_count"]
                - old_result["llm_cc"] / old_result["token_count"]
            )
            path_deltas.append(
                {"path": change["new_path"] or change["old_path"], "change": value}
            )
    regressions = sorted(
        (x for x in path_deltas if x["change"] > 0),
        key=lambda x: (-x["change"], x["path"]),
    )[:10]
    improvements = sorted(
        (x for x in path_deltas if x["change"] < 0),
        key=lambda x: (x["change"], x["path"]),
    )[:10]
    incomplete_coverage = (
        base["totals"]["coverage"] < 1.0 or head["totals"]["coverage"] < 1.0
    )
    status = (
        "failed"
        if errors
        else ("incomplete" if missing or incomplete_coverage else "complete")
    )
    report_changes = []
    for change in plan["changes"]:
        enriched = dict(change)
        old = base_paths.get(change["old_path"])
        new = head_paths.get(change["new_path"])
        enriched["base"] = (
            {
                "category": old["category"],
                "score": (
                    results[old["key"]]["llm_cc"] / results[old["key"]]["token_count"]
                    if old
                    and old.get("key") in results
                    and results[old["key"]]["token_count"]
                    else None
                ),
            }
            if old
            else None
        )
        enriched["head"] = (
            {
                "category": new["category"],
                "score": (
                    results[new["key"]]["llm_cc"] / results[new["key"]]["token_count"]
                    if new
                    and new.get("key") in results
                    and results[new["key"]]["token_count"]
                    else None
                ),
            }
            if new
            else None
        )
        report_changes.append(enriched)
    report = {
        "schema_version": 1,
        "identity": plan["identity"],
        "fingerprint": plan["fingerprint"],
        "status": status,
        "profile": plan["profile"],
        "inventories": plan["inventories"],
        "results": results,
        "sides": {"base": base, "head": head},
        "comparisons": comparisons,
        "changes": report_changes,
        "change_counts": counts,
        "leading_regressions": regressions,
        "leading_improvements": improvements,
        "cache_stats": plan["cache_stats"],
        "errors": errors,
    }
    write_json(output / "report.json", report)
    markdown = _render(report, full=True)
    (output / "report.md").write_text(markdown, encoding="utf-8")
    comment_bytes = _render(report).encode("utf-8")
    if len(comment_bytes) > 24 * 1024:
        comment_bytes = (
            comment_bytes[: 24 * 1024 - 64].decode("utf-8", "ignore").encode("utf-8")
            + b"\n\n_Full details are available in artifacts._\n"
        )
    (output / "comment.md").write_bytes(comment_bytes)
    publication = {
        "schema_version": 1,
        "identity": plan["identity"],
        "fingerprint": plan["fingerprint"],
        "status": status,
        "comment": {
            "path": "comment.md",
            "sha256": hashlib.sha256(comment_bytes).hexdigest(),
            "bytes": len(comment_bytes),
        },
    }
    write_json(output / "publication.json", publication)
    return report


def failure_report(output_dir, identity, fingerprint, errors):
    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    identity_fields = (
        "repository",
        "pipeline_id",
        "head_sha",
        "target_sha",
        "base_sha",
        "target_branch",
        "pr_number",
        "started_at",
    )
    normalized_identity = {key: (identity or {}).get(key) for key in identity_fields}
    unavailable = {
        "score": _delta(None, None),
        "raw_llm_cc": _delta(None, None),
        "tokens": _delta(None, None),
        "coverage": {"base": 0.0, "head": 0.0},
    }
    report = {
        "schema_version": 1,
        "identity": normalized_identity,
        "fingerprint": fingerprint,
        "status": "failed",
        "profile": None,
        "inventories": {"base": [], "head": []},
        "results": {},
        "sides": {"base": _side([], {}), "head": _side([], {})},
        "comparisons": {
            name: dict(unavailable)
            for name in ("runtime", "tests", "tooling", "repository")
        },
        "changes": [],
        "change_counts": {
            "additions": 0,
            "deletions": 0,
            "renames": 0,
            "category_moves": 0,
        },
        "leading_regressions": [],
        "leading_improvements": [],
        "cache_stats": {
            "items": 0,
            "hits": 0,
            "misses": 0,
            "hit_bytes": 0,
            "miss_bytes": 0,
        },
        "errors": [str(error) for error in errors],
    }
    write_json(output / "report.json", report)
    markdown = "## llm-cc comparison\n\nStatus: **failed**\n\nErrors:\n" + "".join(
        "- %s\n" % _safe(error) for error in report["errors"]
    )
    (output / "report.md").write_text(markdown, encoding="utf-8")
    raw_comment = markdown.encode("utf-8")
    comment = (
        raw_comment
        if len(raw_comment) <= 24 * 1024
        else raw_comment[: 24 * 1024].decode("utf-8", "ignore").encode("utf-8")
    )
    (output / "comment.md").write_bytes(comment)
    write_json(
        output / "publication.json",
        {
            "schema_version": 1,
            "identity": normalized_identity,
            "fingerprint": fingerprint,
            "status": "failed",
            "comment": {
                "path": "comment.md",
                "sha256": hashlib.sha256(comment).hexdigest(),
                "bytes": len(comment),
            },
        },
    )
    return report


def aggregate(plan_path, worker_paths, output_dir):
    output = Path(output_dir)
    try:
        plan = read_json(plan_path)
        return _aggregate(plan, worker_paths, output)
    except Exception as exc:
        plan = locals().get("plan", {})
        if not isinstance(plan, dict):
            plan = {}
        return failure_report(
            output,
            plan.get("identity"),
            plan.get("fingerprint"),
            ["aggregation failed: %s" % exc],
        )


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
):
    from .worker import run_worker

    if isinstance(scorer, os.PathLike):
        scorer = str(scorer)
    plan = prepare(
        repo, head, target, identity, profile, rules, cache, output_dir, max_workers
    )
    if plan["workers"] and not all((scorer, model, installed_root)):
        return failure_report(
            output_dir,
            plan["identity"],
            plan["fingerprint"],
            ["cache misses require --scorer, --model, and --installed-root"],
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
        )
        worker_paths.append(Path(output_dir) / ("worker-%d.json" % worker["worker_id"]))
    return aggregate(Path(output_dir) / "plan.json", worker_paths, output_dir)
