#!/usr/bin/env python3
"""Compare rankings and historical edit responses without equating agreement to quality."""
import argparse
import csv
import json
import math
from pathlib import Path
import random
import statistics

BASELINE = "deepseek-v2-lite-q6"
METRICS = ["lmcc_per_token", "llm_cc", "mean_entropy", "density"]

def ranks(values):
    ordered = sorted(range(len(values)), key=values.__getitem__)
    result = [0.] * len(values)
    first = 0
    while first < len(values):
        end = first + 1
        while end < len(values) and values[ordered[end]] == values[ordered[first]]:
            end += 1
        for index in ordered[first:end]:
            result[index] = (first + end - 1) / 2
        first = end
    return result

def pearson(xs, ys):
    mx, my = statistics.mean(xs), statistics.mean(ys)
    dx, dy = [x - mx for x in xs], [y - my for y in ys]
    denominator = math.sqrt(sum(x*x for x in dx) * sum(y*y for y in dy))
    return sum(x*y for x, y in zip(dx, dy)) / denominator if denominator else None

def spearman(xs, ys):
    return pearson(ranks(xs), ranks(ys))

def partial_spearman(xs, ys, sizes):
    xy, xz, yz = spearman(xs, ys), spearman(xs, sizes), spearman(ys, sizes)
    if any(x is None for x in (xy, xz, yz)):
        return None
    denominator = math.sqrt(max(0., (1-xz*xz) * (1-yz*yz)))
    return (xy-xz*yz) / denominator if denominator > 1e-12 else None

def bootstrap(xs, ys, repeats=2000):
    rng = random.Random(20260905)
    values = []
    for _ in range(repeats):
        indices = [rng.randrange(len(xs)) for _ in xs]
        value = spearman([xs[i] for i in indices], [ys[i] for i in indices])
        if value is not None:
            values.append(value)
    values.sort()
    return [values[int((len(values)-1)*p)] for p in (0.025, 0.975)]

def compare(reference, candidate, ids, metric, sizes):
    xs, ys = [reference[x][metric] for x in ids], [candidate[x][metric] for x in ids]
    top = min(5, len(ids))
    tx = set(sorted(ids, key=lambda k: reference[k][metric], reverse=True)[:top])
    ty = set(sorted(ids, key=lambda k: candidate[k][metric], reverse=True)[:top])
    return dict(n=len(ids), spearman=spearman(xs, ys), bootstrap_95=bootstrap(xs, ys),
                partial_spearman_bytes=partial_spearman(xs, ys, [sizes[x] for x in ids]),
                top5_overlap=len(tx & ty),
                median_relative_difference=statistics.median(
                    abs(y-x)/abs(x) for x, y in zip(xs, ys) if x != 0))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent)
    args = parser.parse_args()
    root = args.root.resolve()
    corpus = json.loads((root / "corpus.json").read_text())["files"]
    sizes = {row["id"]: row["bytes"] for row in corpus}
    heldout = [row["id"] for row in corpus if row["group"] == "heldout"]
    runs = {}
    for path in (root / "results").glob("*.json"):
        record = json.loads(path.read_text())
        if not record["valid"]:
            raise RuntimeError(f"Invalid result: {path}")
        runs[record["model"], record["label"]] = record
    models = json.loads((root / "models.json").read_text())
    baseline = runs[BASELINE, "default-r1"]
    ref = {row["id"]: row for row in baseline["files"]}
    summary, file_rows, pair_rows = [], [], []
    for model in models:
        name = model["name"]
        initial = runs[name, "default-r1"]
        files = {row["id"]: row for row in initial["files"]}
        repeated = [record for (n, label), record in runs.items()
                    if n == name and label.startswith("default-r")]
        timing = [record["wall_seconds"] for record in repeated]
        item = dict(model=name, bytes=model["bytes"],
                    wall_seconds=timing, median_seconds=statistics.median(timing),
                    peak_gpu_mib=max(record["peak_gpu_mib"] or 0 for record in repeated),
                    token_count=initial["totals"][0]["token_count"],
                    heldout={metric: compare(ref, files, heldout, metric, sizes) for metric in METRICS})
        # Repetitions also expose any nondeterministic changes in scores.
        item["max_repeat_score_difference"] = max(
            abs(row["lmcc_per_token"] - files[row["id"]]["lmcc_per_token"])
            for record in repeated for row in record["files"])
        common_functions = []
        hotspot_overlap = []
        for identifier in heldout:
            functions = {(f["name"], f["start_line"], f["end_line"]): f for f in files[identifier]["functions"]}
            for f in ref[identifier]["functions"]:
                other = functions.get((f["name"], f["start_line"], f["end_line"]))
                if other and min(f["token_count"], other["token_count"]) >= 100:
                    common_functions.append((f["lmcc_per_token"], other["lmcc_per_token"]))
            first = {h["line"] for h in ref[identifier]["hotspots"][:5]}
            second = {h["line"] for h in files[identifier]["hotspots"][:5]}
            if first:
                hotspot_overlap.append(len(first & second) / len(first))
        item["functions_at_least_100_tokens"] = dict(
            n=len(common_functions), spearman=spearman(*zip(*common_functions)) if len(common_functions) >= 2 else None)
        item["mean_top5_hotspot_overlap"] = statistics.mean(hotspot_overlap)
        item["sensitivity"] = {}
        for label in ["tau-0.4", "tau-1.0", "percentile-80", "structural-only", "reference"]:
            alternative = {row["id"]: row for row in runs[name, label]["files"]}
            matching_ref = {row["id"]: row for row in runs[BASELINE, label]["files"]}
            item["sensitivity"][label] = dict(
                vs_same_setting_baseline=spearman([matching_ref[k]["lmcc_per_token"] for k in heldout],
                                                  [alternative[k]["lmcc_per_token"] for k in heldout]),
                vs_own_default=spearman([files[k]["lmcc_per_token"] for k in heldout],
                                        [alternative[k]["lmcc_per_token"] for k in heldout]))
        for row in initial["files"]:
            file_rows.append(dict(model=name, id=row["id"], tokens=row["token_count"],
                                  **{key: row[key] for key in METRICS}))
        for before in [key for key in files if key.startswith("before/")]:
            after = before.replace("before/", "after/", 1)
            for metric in METRICS:
                b, a = files[before][metric], files[after][metric]
                pair_rows.append(dict(model=name, source=before.removeprefix("before/"), metric=metric,
                                      before=b, after=a, delta=a-b, percent=100*(a-b)/b if b else None))
        summary.append(item)
    (root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    for filename, rows in [("file-scores.csv", file_rows), ("edit-deltas.csv", pair_rows)]:
        with (root / filename).open("w") as output:
            writer = csv.DictWriter(output, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    for item in summary:
        lmcc = item["heldout"]["lmcc_per_token"]
        print(f"{item['model']:24s} {item['bytes']/1e9:5.2f} GB  {item['median_seconds']:6.2f}s  "
              f"{item['peak_gpu_mib']:5d} MiB  rho={lmcc['spearman']:.3f}  "
              f"top5={lmcc['top5_overlap']}/5  CI={lmcc['bootstrap_95']}")

if __name__ == "__main__":
    main()
