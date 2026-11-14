#!/usr/bin/env python3
"""Tables of the PDF/UA-1 tool comparison (dox-validate, VellumPdf.Cli, outloud) from a cold block of bench.py.

    python3 -I bench/analyze_ua1_tools.py bench/results/ua1-tools-2026-11-14

Same definitions as bench/analyze.py: only documents that every variant processed in every repetition; per document
the median wall time of the repetitions; per group the median over its documents; time relative to the full
dox-validate run as the median of the per-document ratios (geometric mean in brackets); documents/s and pages/s from
the sum of the per-document medians (one process at a time); peak RSS of the tool process (largest of the
repetitions per document; median and maximum over the documents). Writes tables.md next to the input. Standard
library only.
"""
from __future__ import annotations

import collections
import csv
import math
import statistics
import sys
from pathlib import Path

VARIANTS = ["dox", "dox-ua1rules", "vellum-ua1", "outloud-conf"]
NAMES = {"dox": "dox-validate (full catalogue)", "dox-ua1rules": "dox-validate --rules EQX-UA1-",
         "vellum-ua1": "VellumPdf 2.3.3 -p ua1", "outloud-conf": "outloud 0.2.1 --layer conformance"}


def fmt(s: float) -> str:
    return f"{s * 1000:.3g} ms" if s < 1 else f"{s:.3g} s"


def main(res: Path) -> None:
    corpus = {r["id"]: r for r in csv.DictReader(open(res / "corpus.csv"))}
    runs: dict = collections.defaultdict(lambda: collections.defaultdict(list))
    excluded: collections.Counter = collections.Counter()
    for r in csv.DictReader(open(res / "cold.csv")):
        ok = r["n_failed"] in ("0", "") and r["timeout"] in ("False", "0", "") and r["n_ok"] != "0"
        if ok:
            runs[r["item"]][r["variant"]].append((float(r["wall_s"]), int(r["maxrss_kb"] or 0) / 1024))
        else:
            excluded[(r["variant"], r["item"])] += 1
    reps = max(len(x) for d in runs.values() for x in d.values())
    docs = sorted(d for d in runs if all(len(runs[d][v]) == reps for v in VARIANTS))

    def med(v, d):
        return statistics.median(w for w, _ in runs[d][v])

    def rss(v, d):
        return max(m for _, m in runs[d][v])

    groups = {"all": docs, **{t: [d for d in docs if corpus[d]["tier"] == t] for t in ("small", "medium", "large")}}
    out = [f"# PDF/UA-1 tool comparison, cold start (one process per document)\n",
           f"{len(docs)} documents processed by every variant in all {reps} repetitions. Excluded runs (variant, document):",
           ", ".join(f"{v} {d} ({n}x)" for (v, d), n in sorted(excluded.items())) + ".\n",
           "| group | tool | docs | pages | median wall | IQR | docs/s | pages/s | median peak RSS | max peak RSS | "
           "time ÷ dox-validate (full) |", "|---|---|---|---|---|---|---|---|---|---|---|"]
    for g, ds in groups.items():
        pages = sum(int(corpus[d]["pages"]) for d in ds)
        for v in VARIANTS:
            meds = [med(v, d) for d in ds]
            q = statistics.quantiles(meds, n=4) if len(ds) > 3 else [min(meds), 0, max(meds)]
            ratios = [med(v, d) / med("dox", d) for d in ds]
            gm = math.exp(statistics.mean(math.log(x) for x in ratios))
            tot = sum(meds)
            rel = "1 (reference)" if v == "dox" else f"{statistics.median(ratios):.3g}× ({gm:.3g}×)"
            out.append(f"| {g} | {NAMES[v]} | {len(ds)} | {pages} | {fmt(statistics.median(meds))} | "
                       f"{fmt(q[0])}–{fmt(q[2])} | {len(ds) / tot:.3g} | {pages / tot:,.4g} | "
                       f"{statistics.median(rss(v, d) for d in ds):.1f} MB | {max(rss(v, d) for d in ds):,.0f} MB | {rel} |")
    (res / "tables.md").write_text("\n".join(out) + "\n")
    print("\n".join(out))


if __name__ == "__main__":
    main(Path(sys.argv[1]))
