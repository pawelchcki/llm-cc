#!/usr/bin/env python3
"""Report each model's pooled 67th-percentile entropy (the paper's tau rule)."""
import argparse
import bisect
import gzip
import hashlib
import json
import math
from pathlib import Path
import statistics

from reference_anchor import HF_MODEL, HF_REVISION, REFERENCE_DTYPE

PERCENTILE = 67.0
PAPER_TAU = 0.67


def percentile(values, rank_percent):
    # Same linear interpolation as llmcc::Percentile in src/core.cc.
    ordered = sorted(values)
    rank = rank_percent / 100.0 * (len(ordered) - 1)
    lower, upper = math.floor(rank), math.ceil(rank)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (rank - lower)


def inverse_percentile(values, target):
    """The percentile whose `percentile()` value is exactly `target`.

    The empirical fraction below `target` is not this inverse: `percentile()`
    interpolates between the two values bracketing the rank, so the matched
    quantile has to be solved on the same interpolation.
    """
    ordered = sorted(values)
    if target <= ordered[0]:
        return 0.0
    if target >= ordered[-1]:
        return 100.0
    upper = bisect.bisect_left(ordered, target)
    lower = upper - 1
    span = ordered[upper] - ordered[lower]
    rank = lower + (1.0 if span == 0 else (target - ordered[lower]) / span)
    return 100.0 * rank / (len(ordered) - 1)


def pooled_values(entropies):
    return [value for values in entropies.values() for value in values if value is not None]


def describe(entropies, matched_percentile=None):
    pooled = pooled_values(entropies)
    per_file = [percentile([v for v in values if v is not None], PERCENTILE)
                for values in entropies.values() if any(v is not None for v in values)]
    return dict(programs=len(entropies), tokens=len(pooled),
                tau_p67=round(percentile(pooled, PERCENTILE), 4),
                mean_entropy=round(statistics.fmean(pooled), 4),
                fraction_at_or_above_paper_tau=round(
                    sum(value >= PAPER_TAU for value in pooled) / len(pooled), 4),
                median_per_file_p67=round(statistics.median(per_file), 4),
                **({} if matched_percentile is None else dict(
                    tau_matched=round(percentile(pooled, matched_percentile), 4))))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent)
    args = parser.parse_args()
    root = args.root.resolve()
    records = {}
    for path in sorted((root / "results").glob("*.entropy.json.gz")):
        with gzip.open(path, "rt") as stream:
            records[path] = json.load(stream)
    # The paper's 0.67 is CodeLlama's 67th percentile on the authors' full
    # corpus. On this corpus the reference CodeLlama reaches 0.67 at a slightly
    # different percentile; transferring that quantile keeps CodeLlama exactly
    # at the paper's operating point and gives every model the same share of
    # boundary tokens.
    anchor = next((r for r in records.values() if "llm_cc" in r), None)
    matched = None
    if anchor is not None:
        # A reused anchor must come from the current corpus and reference code,
        # and from the verified checkpoint and dtype the operating point is
        # defined by, or every matched tau would mix inputs. A copied checkpoint
        # records `revision: null` and is refused here.
        manifest = json.loads((root / "corpus.json").read_text())
        expected = dict(corpus_sha256=hashlib.sha256(
                            json.dumps(manifest["files"], sort_keys=True).encode()).hexdigest(),
                        reference_commit=manifest["revision"],
                        model=HF_MODEL, revision=HF_REVISION,
                        dtype=REFERENCE_DTYPE)
        recorded = {key: anchor.get(key) for key in expected}
        if recorded != expected:
            raise SystemExit(f"reference anchor does not match corpus.json (recorded {recorded}, "
                             f"expected {expected}); rerun reference_anchor.py")
        matched = inverse_percentile(pooled_values(anchor["entropies"]), PAPER_TAU)
    summary = dict(percentile=PERCENTILE, matched_percentile=matched and round(matched, 3),
                   models={})
    for path, record in records.items():
        name = path.name.removesuffix(".entropy.json.gz")
        item = describe(record["entropies"], matched)
        if "llm_cc" in record:
            item["mean_reference_llm_cc"] = round(statistics.fmean(record["llm_cc"].values()), 3)
        analysis = root / "results" / (name + ".analysis.json")
        if analysis.exists():
            result = json.loads(analysis.read_text())
            # An analysis is reported only when it came from the same inputs as
            # the entropies beside it, at the tau those entropies now imply.
            # Thresholds are summarized before `run.py analysis` reruns, so a
            # stale artifact is skipped with a notice rather than ending the run.
            if result.get("inputs") != record.get("inputs"):
                stale = f"was produced from different or unrecorded inputs than {path.name}"
            elif result.get("tau") != item.get("tau_matched"):
                stale = f"used tau {result.get('tau')}, not the matched {item.get('tau_matched')}"
            else:
                stale = None
            if stale:
                print(f"skipping {analysis.name}: it {stale}; rerun run.py analysis",
                      flush=True)
            else:
                item["analysis_tau"] = result["tau"]
                item["mean_llm_cc_per_program"] = round(
                    result["totals"]["mean_llm_cc_per_file"], 3)
        summary["models"][name] = item
    (root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"matched percentile: {summary['matched_percentile']}")
    print(f"{'model':36s} {'tokens':>7s} {'tau_p67':>8s} {'matched':>8s} {'>=0.67':>7s}")
    for name, item in summary["models"].items():
        print(f"{name:36s} {item['tokens']:7d} {item['tau_p67']:8.4f} "
              f"{item.get('tau_matched', float('nan')):8.4f} "
              f"{item['fraction_at_or_above_paper_tau']:7.3f}")


if __name__ == "__main__":
    main()
