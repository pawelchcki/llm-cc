import datetime
import hashlib
import json
import os
import re
import unicodedata
from pathlib import Path

from .cache import FilesystemStore
from .common import (
    SCHEMA_VERSION,
    digest,
    read_json,
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

COMMENT_LIMIT = 24 * 1024
REPOSITORY_RULES_PATH = ".llm-cc/comparison-rules.json"


def _utc_now():
    return (
        datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00", "Z")
    )


def resolve_rules(repo, target, rules, repository_rules_path):
    """Prefer the target commit's own rules; a PR cannot reclassify itself."""
    if repository_rules_path:
        raw = read_tree_file(repo, target, repository_rules_path)
        if raw is not None:
            try:
                candidate = json.loads(raw.decode("utf-8"))
            except (UnicodeError, ValueError) as error:
                raise ValueError(
                    "repository classification rules at %s are not valid JSON: %s"
                    % (repository_rules_path, error)
                ) from None
            return (
                validate_rules(candidate),
                {
                    "source": "repository",
                    "path": repository_rules_path,
                    "commit": target,
                },
            )
    return validate_rules(rules or {}), {"source": "host"}


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
):
    validate_execution_policy(profile)
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
    hits = {}
    misses = []
    for key, item in sorted(items.items()):
        cached = cache.get(item, fingerprint) if cache else None
        if cached is not None and valid_result(cached, item, fingerprint):
            hits[key] = cached
        else:
            misses.append(item)
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
    validate_execution_policy(plan["profile"])
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


def _neutralize_controls(text):
    """Remove control/format characters that could reorder or hide rendered text."""
    value = str(text).encode("utf-8", "backslashreplace").decode("utf-8")
    return "".join(
        character
        if unicodedata.category(character) not in {"Cc", "Cf"}
        and character not in "\u2028\u2029"
        else "\\u%04x" % ord(character)
        for character in value
    )


def _safe(text):
    value = _neutralize_controls(text)
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


PATH_DISPLAY_LIMIT = 120


def _code(text, table=False):
    """Render untrusted text as a balanced code span that Markdown cannot escape."""
    value = _neutralize_controls(text)
    if len(value) > PATH_DISPLAY_LIMIT:
        head = (PATH_DISPLAY_LIMIT - 1) // 2
        tail = PATH_DISPLAY_LIMIT - 1 - head
        value = value[:head] + "…" + value[len(value) - tail :]
    if table:
        # GFM splits table cells on every unescaped pipe, code spans included.
        # Double any backslash run already in front of a pipe: "a\\|b" would
        # otherwise escape the backslash and leave the pipe splitting the cell.
        # Backslashes elsewhere are literal and stay as they are.
        value = re.sub(
            r"(\\*)\|", lambda match: "\\" * (2 * len(match.group(1))) + r"\|", value
        )
    longest = run = 0
    for character in value:
        run = run + 1 if character == "`" else 0
        longest = max(longest, run)
    fence = "`" * (longest + 1)
    pad = " " if not value or value.startswith("`") or value.endswith("`") else ""
    return fence + pad + value + pad + fence


TRUNCATION_NOTICE = b"\n_Full details are available in artifacts._\n"


def _assemble(sections, limit=COMMENT_LIMIT):
    """Drop the least important sections, then whole lines, to fit the limit."""

    def render(kept):
        lines = []
        for _, section in kept:
            lines.extend(section)
        return ("\n".join(lines).rstrip("\n") + "\n").encode("utf-8")

    kept = list(sections)
    payload = render(kept)
    while len(payload) > limit and len({priority for priority, _ in kept}) > 1:
        lowest = max(priority for priority, _ in kept)
        kept = [entry for entry in kept if entry[0] != lowest]
        payload = render(kept)
    if len(payload) > limit:
        head = payload[: limit - len(TRUNCATION_NOTICE)]
        cut = head.rfind(b"\n")
        payload = (head[: cut + 1] if cut >= 0 else head) + TRUNCATION_NOTICE
    return payload


def _fmt(value):
    return "unavailable" if value is None else "%.6g" % value


def _raw(value, sign=""):
    return "unavailable" if value is None else ("%" + sign + ".1f") % value


def _percent(value):
    return "unavailable" if value is None else "%+.3g%%" % value


def _path_change(delta):
    if "raw_change" not in delta:  # Reports written before raw headlines.
        return "%+.6g LM-CC/token" % delta["change"]
    if delta["change"] is None:  # Zero tokens on one side.
        return "%+.1f LM-CC" % delta["raw_change"]
    return "%+.1f LM-CC (%+.3g per token)" % (delta["raw_change"], delta["change"])


def _headline(report):
    lines = []
    for name in ("runtime", "tests", "tooling", "repository"):
        item = (report.get("comparisons") or {}).get(name)
        if not item:
            continue
        raw = item["raw_llm_cc"]
        lines.append(
            "- %s %s → %s LM-CC (%s)"
            % (
                name,
                _raw(raw["base"]),
                _raw(raw["head"]),
                _percent(raw["percent"]),
            )
        )
    return lines


def _category_table(report):
    lines = [
        "",
        "| Category | LM-CC base | LM-CC head | LM-CC Δ | LM-CC Δ% | LM-CC/token Δ | Tokens Δ | Coverage |",
        "|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for name in ("runtime", "tests", "tooling", "repository"):
        item = (report.get("comparisons") or {}).get(name)
        if not item:
            continue
        lines.append(
            "| %s | %s | %s | %s | %s | %s | %s | %.1f%% / %.1f%% |"
            % (
                name,
                _raw(item["raw_llm_cc"]["base"]),
                _raw(item["raw_llm_cc"]["head"]),
                _raw(item["raw_llm_cc"]["absolute"], "+"),
                _percent(item["raw_llm_cc"]["percent"]),
                _fmt(item["score"]["absolute"]),
                _fmt(item["tokens"]["absolute"]),
                item["coverage"]["base"] * 100,
                item["coverage"]["head"] * 100,
            )
        )
    return lines


def _changed_rows(report):
    rows = list(report.get("changed_files") or [])
    rows.sort(
        key=lambda row: (
            0 if row["delta"] is not None else 1,
            -abs(row["delta"] or 0.0),
            row["path"],
        )
    )
    return rows


def _changed_table_lines(rows, total_head):
    lines = [
        "| Path | Category | Base | Head | Δ | Δ% | Head rank |",
        "|---|---|---:|---:|---:|---:|---:|",
    ]
    for row in rows:
        side = row["head"] or row["base"] or {}
        rank = "—"
        if row["head"] and row["head"].get("rank") is not None:
            rank = "#%d/%d" % (row["head"]["rank"], total_head)
        # An added or deleted path has no opposite side at all, which reads
        # differently from a present file whose score could not be measured.
        lines.append(
            "| %s | %s | %s | %s | %s | %s | %s |"
            % (
                _code(row["path"], table=True),
                side.get("category") or "—",
                "—" if row["base"] is None else _fmt(row["base"]["score"]),
                "—" if row["head"] is None else _fmt(row["head"]["score"]),
                "—"
                if row["base"] is None or row["head"] is None
                else _fmt(row["delta"]),
                "—"
                if row["base"] is None or row["head"] is None
                else _percent(row["percent"]),
                rank,
            )
        )
    return lines


CHANGED_FILES_INLINE = 25


def _comment_sections(report, full=False):
    identity = report.get("identity") or {}
    rankings = report.get("rankings") or {"base": [], "head": []}
    sections = [
        (
            1,
            ["## llm-cc comparison", "", "Status: **%s**" % report["status"], ""]
            + _headline(report),
        ),
        (2, _category_table(report)),
    ]
    status_lines = [
        "",
        "Cache: %d hits, %d misses."
        % (report["cache_stats"]["hits"], report["cache_stats"]["misses"]),
    ]
    if report["errors"]:
        status_lines += ["", "Errors:"] + [
            "- " + _code(error) for error in report["errors"][:20]
        ]
    sections.append((3, status_lines))

    rows = _changed_rows(report)
    total_head = len(rankings.get("head") or [])
    if rows:
        inline = rows[:CHANGED_FILES_INLINE]
        lines = ["", "### Changed files", ""] + _changed_table_lines(inline, total_head)
        if not full and len(rows) > len(inline):
            lines += [
                "",
                "_%d more changed files are listed in the full report._"
                % (len(rows) - len(inline)),
            ]
        sections.append((4, lines))

    for title, entries in (
        ("Leading regressions", report.get("leading_regressions") or []),
        ("Leading improvements", report.get("leading_improvements") or []),
    ):
        if entries:
            sections.append(
                (
                    5,
                    ["", "### " + title, ""]
                    + [
                        "- %s: %s" % (_code(entry["path"]), _path_change(entry))
                        for entry in entries
                    ],
                )
            )

    offenders = (rankings.get("base") or [])[:10]
    if offenders:
        # rankings["base"] is the merge base, not the target branch tip. Label
        # it as such: a branch behind its target would otherwise present stale
        # scores as the target branch's current state.
        base = identity.get("base_sha") or "base"
        lines = [
            "",
            "<details>",
            "<summary>Top offenders on the merge base (%s)</summary>" % _code(base),
            "",
            "| # | Path | Category | Score | Touched |",
            "|---:|---|---|---:|---|",
        ]
        for entry in offenders:
            lines.append(
                "| %d | %s | %s | %s | %s |"
                % (
                    entry["rank"],
                    _code(entry["path"], table=True),
                    entry["category"],
                    _fmt(entry["score"]),
                    "yes" if entry["changed"] else "",
                )
            )
        lines += ["", "</details>"]
        sections.append((6, lines))

    links = (report.get("presentation") or {}).get("report_links") or {}
    baseline = links.get("baseline")
    if isinstance(baseline, str) and baseline.startswith("https://"):
        sections.append((7, ["", "Baseline ranking: <%s>" % baseline]))

    source = report.get("rules_source") or {}
    if source.get("source") == "repository":
        sections.append(
            (
                8,
                [
                    "",
                    "Rules: repository %s@%s"
                    % (_code(source.get("path")), (source.get("commit") or "")[:7]),
                ],
            )
        )
    elif source.get("source") == "host":
        sections.append((8, ["", "Rules: host"]))
    return sections


def _full_sections(report):
    rows = _changed_rows(report)
    total_head = len(((report.get("rankings") or {}).get("head")) or [])
    lines = []
    if len(rows) > CHANGED_FILES_INLINE:
        lines += [
            "",
            "<details>",
            "<summary>All %d changed files</summary>" % len(rows),
            "",
        ]
        lines += _changed_table_lines(rows[CHANGED_FILES_INLINE:], total_head)
        lines += ["", "</details>"]
    lines += [
        "",
        "### Raw totals",
        "",
        "| Side/category | LM-CC | Tokens | LM-CC/token |",
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
                    _raw(values["llm_cc"]),
                    _fmt(values["token_count"]),
                    _fmt(values["score"]),
                )
            )
    lines += ["", "### Coverage details", ""]
    for side in ("base", "head"):
        totals = report["sides"][side]["totals"]
        reasons = (
            ", ".join(
                "%s: %d" % (key, value)
                for key, value in sorted(totals["unmeasured_reasons"].items())
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
            old = _code(change["old_path"]) if change["old_path"] is not None else "∅"
            new = _code(change["new_path"]) if change["new_path"] is not None else "∅"
            lines.append("- %s → %s (%s)" % (old, new, change["status"]))
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
                    _code(file["path"], table=True),
                    file["category"],
                    file["language"] or "—",
                    file["size"] if file["size"] is not None else "—",
                    measurement,
                )
            )
    return lines


def _render(report, full=False):
    sections = _comment_sections(report, full)
    if not full:
        return _assemble(sections).decode("utf-8")
    lines = []
    for _, section in sections:
        lines.extend(section)
    lines.extend(_full_sections(report))
    return "\n".join(lines).rstrip("\n") + "\n"


def _render_baseline(report, output):
    """Write the standalone worst-offender view for the scored head revision."""
    identity = report.get("identity") or {}
    rankings = (report.get("rankings") or {}).get("head") or []
    categories = report["sides"]["head"]["categories"]
    lines = [
        "Baseline ranking for %s@%s (%s)"
        % (
            identity.get("repository") or "unknown",
            identity.get("head_sha") or "unknown",
            identity.get("target_branch") or "unknown",
        ),
        "",
        "Status: **%s**" % report["status"],
        "",
        "| Category | Score | Raw LLM | Tokens | Measured paths |",
        "|---|---:|---:|---:|---:|",
    ]
    for name in ("runtime", "tests", "tooling"):
        values = categories[name]
        lines.append(
            "| %s | %s | %s | %s | %d/%d |"
            % (
                name,
                _fmt(values["score"]),
                _fmt(values["llm_cc"]),
                _fmt(values["token_count"]),
                values["measured_paths"],
                values["supported_paths"],
            )
        )
    totals = report["sides"]["head"]["totals"]
    lines.append(
        "| repository | %s | %s | %s | %d/%d |"
        % (
            _fmt(totals["score"]),
            _fmt(totals["llm_cc"]),
            _fmt(totals["token_count"]),
            totals["measured_paths"],
            totals["supported_paths"],
        )
    )

    def table(entries):
        rows = [
            "| # | Path | Category | Score | LLM | Tokens |",
            "|---:|---|---|---:|---:|---:|",
        ]
        for entry in entries:
            rows.append(
                "| %d | %s | %s | %s | %s | %d |"
                % (
                    entry["rank"],
                    _code(entry["path"], table=True),
                    entry["category"],
                    _fmt(entry["score"]),
                    _fmt(entry["llm_cc"]),
                    entry["token_count"],
                )
            )
        return rows

    lines += ["", "## Top 50 files", ""]
    lines += table(rankings[:50]) if rankings else ["No measured files."]
    for name in ("runtime", "tests", "tooling"):
        selected = [entry for entry in rankings if entry["category"] == name][:20]
        lines += ["", "## Top 20 %s files" % name, ""]
        lines += table(selected) if selected else ["No measured files."]
    reasons = totals["unmeasured_reasons"]
    lines += ["", "## Unmeasured paths", ""]
    if reasons:
        lines += [
            "- %s: %d" % (reason, count) for reason, count in sorted(reasons.items())
        ]
    else:
        lines.append("- None.")
    (output / "baseline.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    write_json(
        output / "baseline.json",
        {
            "schema_version": 1,
            "identity": report["identity"],
            "fingerprint": report["fingerprint"],
            "status": report["status"],
            "categories": categories,
            "rankings": rankings,
        },
    )


def _rankings(inventory, results, changed_paths):
    """Order every measured file by descending score for a stable repository view."""
    entries = []
    for file in inventory:
        if not file["scorable"]:
            continue
        result = results.get(file["key"])
        if not result or not result["token_count"]:
            continue
        entries.append(
            {
                "path": file["path"],
                "category": file["category"],
                "language": file["language"],
                "size": file["size"],
                "score": result["llm_cc"] / result["token_count"],
                "llm_cc": result["llm_cc"],
                "token_count": result["token_count"],
                "rank": 0,
                "category_rank": 0,
                "changed": file["path"] in changed_paths,
            }
        )
    entries.sort(key=lambda entry: (-entry["score"], entry["path"]))
    seen = {}
    for position, entry in enumerate(entries, 1):
        entry["rank"] = position
        seen[entry["category"]] = seen.get(entry["category"], 0) + 1
        entry["category_rank"] = seen[entry["category"]]
    return entries


def _changed_files(plan_changes, base_paths, head_paths, results, head_rankings):
    """One row per change that carries a score on either side of the comparison."""
    ranked = {entry["path"]: entry for entry in head_rankings}

    def score(file):
        result = results.get(file["key"]) if file and file.get("key") else None
        if not result or not result["token_count"]:
            return None
        return result["llm_cc"] / result["token_count"]

    rows = []
    for change in plan_changes:
        old = base_paths.get(change["old_path"])
        new = head_paths.get(change["new_path"])
        base_score, head_score = score(old), score(new)
        if base_score is None and head_score is None:
            continue
        path = change["new_path"] or change["old_path"]
        delta = _delta(base_score, head_score)
        rank = ranked.get(new["path"]) if new else None
        rows.append(
            {
                "status": change["status"],
                "old_path": change["old_path"],
                "new_path": change["new_path"],
                "path": path,
                "base": None
                if old is None
                else {"category": old["category"], "score": base_score},
                "head": None
                if new is None
                else {
                    "category": new["category"],
                    "score": head_score,
                    "rank": rank["rank"] if rank else None,
                    "category_rank": rank["category_rank"] if rank else None,
                },
                "delta": delta["absolute"],
                "percent": delta["percent"],
            }
        )
    rows.sort(key=lambda row: row["path"])
    return rows



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
    base_changed = {
        change["old_path"] for change in plan["changes"] if change["old_path"]
    }
    head_changed = {
        change["new_path"] for change in plan["changes"] if change["new_path"]
    }
    rankings = {
        "base": _rankings(plan["inventories"]["base"], results, base_changed),
        "head": _rankings(plan["inventories"]["head"], results, head_changed),
    }
    changed_files = _changed_files(
        plan["changes"], base_paths, head_paths, results, rankings["head"]
    )
    def raw_llm_cc(paths, path):
        file = paths.get(path)
        result = results.get(file["key"]) if file and file.get("key") else None
        return None if result is None else result["llm_cc"]

    # Raw LM-CC is the paper's quantity and the displayed headline. It stays
    # defined for zero-token files, whose per-token `change` is null; `change`
    # keeps the per-token delta for existing report consumers.
    path_deltas = []
    for row in changed_files:
        base_raw = raw_llm_cc(base_paths, row["old_path"])
        head_raw = raw_llm_cc(head_paths, row["new_path"])
        if base_raw is not None and head_raw is not None:
            path_deltas.append(
                {
                    "path": row["path"],
                    "change": row["delta"],
                    "raw_change": head_raw - base_raw,
                }
            )
    regressions = sorted(
        (x for x in path_deltas if x["raw_change"] > 0),
        key=lambda x: (-x["raw_change"], x["path"]),
    )[:10]
    improvements = sorted(
        (x for x in path_deltas if x["raw_change"] < 0),
        key=lambda x: (x["raw_change"], x["path"]),
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
        "rankings": rankings,
        "changed_files": changed_files,
        "leading_regressions": regressions,
        "leading_improvements": improvements,
        "rules_source": plan.get("rules_source") or {"source": "host"},
        "presentation": plan.get("presentation") or {},
        "cache_stats": plan["cache_stats"],
        "errors": errors,
    }
    write_json(output / "report.json", report)
    markdown = _render(report, full=True)
    (output / "report.md").write_text(markdown, encoding="utf-8")
    _render_baseline(report, output)
    comment_bytes = _render(report).encode("utf-8")
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
        "rankings": {"base": [], "head": []},
        "changed_files": [],
        "leading_regressions": [],
        "leading_improvements": [],
        "rules_source": {"source": "host"},
        "presentation": {},
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
        "- %s\n" % _code(error) for error in report["errors"]
    )
    (output / "report.md").write_text(markdown, encoding="utf-8")
    _render_baseline(report, output)
    comment = _assemble([(1, markdown.rstrip("\n").split("\n"))])
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
    repository_rules_path=REPOSITORY_RULES_PATH,
    presentation=None,
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
