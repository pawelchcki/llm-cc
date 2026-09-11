#!/usr/bin/env python3
"""Summarize agreement, repeatability, and conservative default gates."""
import argparse
import json
import math
from pathlib import Path
import random
import statistics


def ranks(values):
    order = sorted(range(len(values)), key=values.__getitem__)
    result = [0.0] * len(values)
    start = 0
    while start < len(order):
        end = start + 1
        while end < len(order) and values[order[end]] == values[order[start]]:
            end += 1
        for index in order[start:end]:
            result[index] = (start + end - 1) / 2
        start = end
    return result


def spearman(left, right):
    if len(left) < 2:
        return None
    x, y = ranks(left), ranks(right)
    mx, my = statistics.mean(x), statistics.mean(y)
    dx, dy = [v - mx for v in x], [v - my for v in y]
    denominator = math.sqrt(sum(v * v for v in dx) * sum(v * v for v in dy))
    return sum(a * b for a, b in zip(dx, dy)) / denominator if denominator else None


def bootstrap(left, right, repetitions=2000):
    rng = random.Random(20260910)
    values = []
    for _ in range(repetitions):
        indices = [rng.randrange(len(left)) for _ in left]
        value = spearman([left[i] for i in indices], [right[i] for i in indices])
        if value is not None:
            values.append(value)
    values.sort()
    return ([values[int((len(values) - 1) * 0.025)],
             values[int((len(values) - 1) * 0.975)]] if values else None)


def rows(run):
    result = {}
    for row in run["files"]:
        parts = Path(row["path"]).parts
        marker = max(index for index, part in enumerate(parts) if part == "corpus")
        result["/".join(parts[marker + 1:])] = row
    return result


def common_by_suffix(left, right):
    result = []
    for lpath, lrow in left.items():
        match = next((rrow for rpath, rrow in right.items()
                      if rpath == lpath or rpath.endswith(lpath) or lpath.endswith(rpath)), None)
        if match is not None:
            result.append((lpath, lrow, match))
    return result


def compare(reference_runs, candidate_runs):
    reference, candidate = reference_runs[0], candidate_runs[0]
    pairs = common_by_suffix(rows(reference), rows(candidate))
    left = [a["lmcc_per_token"] for _, a, _ in pairs]
    right = [b["lmcc_per_token"] for _, _, b in pairs]
    top = min(5, len(pairs))
    left_top = {name for name, a, _ in sorted(pairs, key=lambda row: row[1]["lmcc_per_token"],
                                               reverse=True)[:top]}
    right_top = {name for name, _, b in sorted(pairs, key=lambda row: row[2]["lmcc_per_token"],
                                                reverse=True)[:top]}
    function_pairs = []
    hotspot_overlap = []
    for _, a, b in pairs:
        functions = {(f["name"], f["start_line"], f["end_line"]): f
                     for f in b.get("functions", [])}
        for function in a.get("functions", []):
            other = functions.get((function["name"], function["start_line"], function["end_line"]))
            if other and min(function["token_count"], other["token_count"]) >= 100:
                function_pairs.append((function["lmcc_per_token"], other["lmcc_per_token"]))
        first = {spot["line"] for spot in a.get("hotspots", [])[:5]}
        second = {spot["line"] for spot in b.get("hotspots", [])[:5]}
        if first:
            hotspot_overlap.append(len(first & second) / len(first))
    repeatability = 0.0
    for runs in (reference_runs, candidate_runs):
        first_rows = rows(runs[0])
        for repeat in runs[1:]:
            for _, original, repeated in common_by_suffix(first_rows, rows(repeat)):
                repeatability = max(
                    repeatability,
                    abs(original["lmcc_per_token"] - repeated["lmcc_per_token"]),
                )
    reversals = []
    by_name = {name: (a, b) for name, a, b in pairs}
    for name, (before, candidate_before) in by_name.items():
        if "/before/" not in "/" + name:
            continue
        after_name = name.replace("/before/", "/after/")
        if after_name not in by_name:
            continue
        reference_after, candidate_after = by_name[after_name]
        baseline_change = ((reference_after["lmcc_per_token"] - before["lmcc_per_token"])
                           / before["lmcc_per_token"])
        candidate_change = ((candidate_after["lmcc_per_token"] - candidate_before["lmcc_per_token"])
                            / candidate_before["lmcc_per_token"])
        if abs(baseline_change) >= 0.05 and baseline_change * candidate_change < 0:
            reversals.append(name)
    function_rho = (spearman([x for x, _ in function_pairs], [y for _, y in function_pairs])
                    if function_pairs else None)
    file_rho = spearman(left, right)
    return {
        "files": len(pairs), "file_spearman": file_rho,
        "file_bootstrap_95": bootstrap(left, right),
        "eligible_functions": len(function_pairs), "function_spearman": function_rho,
        "top5_retained": len(left_top & right_top), "top5_required": top,
        "mean_hotspot_overlap": statistics.mean(hotspot_overlap) if hotspot_overlap else None,
        "median_relative_score_drift": statistics.median(
            abs(y - x) / abs(x) for x, y in zip(left, right) if x) if left else None,
        "max_repeat_score_difference": repeatability,
        "historical_edit_sign_reversals": reversals,
        "gate_pass": (file_rho is not None and file_rho >= 0.98 and
                      function_rho is not None and function_rho >= 0.98 and
                      len(left_top & right_top) == top and not reversals),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    records = [json.loads(path.read_text()) for path in (args.root / "results").glob("*.json")
               if not path.name.startswith("environment-") and path.name != "summary.json"]
    grouped = {}
    for record in records:
        key = (record["model"], record["backend"], record["scope"])
        grouped.setdefault(key, {}).setdefault(record["label"], []).append(record)
    output = []
    for (model, backend, scope), configurations in sorted(grouped.items()):
        fingerprints = {run.get("invocation_fingerprint")
                        for runs in configurations.values() for run in runs}
        homogeneous = len(fingerprints) == 1 and None not in fingerprints
        item = {"model": model, "backend": backend, "scope": scope,
                "invocation_fingerprint": (next(iter(fingerprints))
                                           if homogeneous else None),
                "complete": {name: sum(run["valid"] for run in runs)
                             for name, runs in configurations.items()},
                "failures": [{"label": name, "repeat": run["repeat"],
                              "class": run["failure_class"], "exit_code": run["exit_code"]}
                             for name, runs in configurations.items() for run in runs
                             if not run["valid"]], "comparisons": {}}
        comparisons = (("flash_attention", "baseline", "flash-f16"),
                       ("q8_kv", "flash-f16", "flash-q8"),
                       ("q4_kv", "flash-f16", "flash-q4"))
        for name, reference_label, candidate_label in comparisons:
            reference = sorted(configurations.get(reference_label, []),
                               key=lambda row: row["repeat"])
            candidate = sorted(configurations.get(candidate_label, []),
                               key=lambda row: row["repeat"])
            repeats_are_unique = (len({run["repeat"] for run in reference}) == len(reference)
                                  and len({run["repeat"] for run in candidate}) == len(candidate))
            if not homogeneous:
                item["comparisons"][name] = {
                    "gate_pass": False,
                    "reason": "mixed or missing invocation fingerprints",
                }
            elif (len(reference) < 3 or len(candidate) < 3 or not repeats_are_unique or
                    not all(row["valid"] for row in reference + candidate)):
                item["comparisons"][name] = {"gate_pass": False,
                                              "reason": "fewer than three distinct complete runs"}
            else:
                item["comparisons"][name] = compare(reference, candidate)
        output.append(item)
    destination = args.root / "summary.json"
    destination.write_text(json.dumps(output, indent=2) + "\n")
    for item in output:
        print(f"{item['model']} {item['backend']} {item['scope']}")
        for name, comparison in item["comparisons"].items():
            print(f"  {name}: {'PASS' if comparison['gate_pass'] else 'FAIL'}")


if __name__ == "__main__":
    main()
