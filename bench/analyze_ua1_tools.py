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

# Table order; a run holds a subset of these (cold.csv decides, see main).
KNOWN = ["dox", "dox-shared", "dox-base", "dox-ua1rules", "dox-base-ua1rules", "vellum-ua1", "outloud-conf"]
# Tools that leave more documents unprocessed; they do not narrow the comparison and get a table of their own.
EXTRA = ["vp-ua1", "cpdf-ua1"]
VARIANTS = ["dox", "dox-ua1rules", "vellum-ua1", "outloud-conf"]
NAMES = {"dox": "dox-validate (full catalogue)", "dox-ua1rules": "dox-validate --rules EQX-UA1-",
         "dox-shared": "dox-validate, default build (shared libqpdf)",
         "dox-base": "dox-validate before the optimisation (729a5c5)",
         "dox-base-ua1rules": "dox-validate before the optimisation, --rules EQX-UA1-",
         "vellum-ua1": "VellumPdf 2.3.3 -p ua1", "outloud-conf": "outloud 0.2.1 --layer conformance",
         "vp-ua1": "veraPDF 1.30.4 -f ua1", "cpdf-ua1": "cpdf 2.9.3 -verify PDF/UA-1(matterhorn)"}


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
    measured = {v for d in runs.values() for v in d} | {v for v, _ in excluded}
    VARIANTS[:] = [v for v in KNOWN if v in measured]
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

    # Further tools of the same cold block (veraPDF, cpdf): each on the documents of the comparison above that it
    # completed in every repetition, with dox-validate's figures on the same documents.
    extras = [v for v in EXTRA if v in measured]
    if extras:
        out += ["", "## Further tools in the same run", "",
                "Each tool on the documents of the comparison above that it processed in every repetition; "
                "dox-validate (full catalogue) on the same documents.", "",
                "| group | tool | docs | median wall | pages/s | median peak RSS | max peak RSS | dox-validate median wall | "
                "dox-validate pages/s | time ÷ dox-validate |", "|---|---|---|---|---|---|---|---|---|---|"]
        for g, all_ds in groups.items():
            for v in extras:
                ds = [d for d in all_ds if len(runs[d][v]) == reps]
                if not ds:
                    continue
                pages = sum(int(corpus[d]["pages"]) for d in ds)
                meds, ref = [med(v, d) for d in ds], [med("dox", d) for d in ds]
                ratios = [a / b for a, b in zip(meds, ref)]
                gm = math.exp(statistics.mean(math.log(x) for x in ratios))
                out.append(f"| {g} | {NAMES[v]} | {len(ds)} of {len(all_ds)} | {fmt(statistics.median(meds))} | "
                           f"{pages / sum(meds):,.4g} | {statistics.median(rss(v, d) for d in ds):.1f} MB | "
                           f"{max(rss(v, d) for d in ds):,.0f} MB | {fmt(statistics.median(ref))} | {pages / sum(ref):,.4g} | "
                           f"{statistics.median(ratios):.3g}× ({gm:.3g}×) |")

    # Per document: peak RSS and median wall time of the large and medium documents.
    if "dox-base" in VARIANTS:
        cols = [v for v in ("dox-base", "dox", "dox-shared", "vellum-ua1", "outloud-conf") if v in VARIANTS]
        out += ["", "## Medium and large documents", "", "Median wall time / peak RSS per document.", "",
                "| document | pages | " + " | ".join(NAMES[v] for v in cols) + " |", "|---|---|" + "---|" * len(cols)]
        big = [d for d in docs if corpus[d]["tier"] in ("medium", "large")]
        for d in sorted(big, key=lambda d: -med("dox-base", d)):
            out.append(f"| {d} | {corpus[d]['pages']} | " +
                       " | ".join(f"{fmt(med(v, d))} / {rss(v, d):.0f} MB" for v in cols) + " |")

    def rows_of(name):
        p = res / name
        return list(csv.DictReader(open(p))) if p.exists() else []

    def good(r):
        return r["timeout"] in ("", "0", "False")

    # Start-up block.
    start = rows_of("startup.csv")
    if start:
        out += ["", "## Start-up", "", "One process each; median over the repetitions (min–max).", "",
                "| tool | call | runs | wall | min–max | CPU (user+sys) | peak RSS |", "|---|---|---|---|---|---|---|"]
        acc: dict = collections.defaultdict(list)
        for r in start:
            acc[(r["variant"], "--version" if r["item"] == "--version" else "smallest corpus file")].append(r)
        for (v, call), rs in sorted(acc.items(), key=lambda kv: (kv[0][1], KNOWN.index(kv[0][0]) if kv[0][0] in KNOWN else 99)):
            w = [float(r["wall_s"]) for r in rs]
            cpu = [float(r["user_s"]) + float(r["sys_s"]) for r in rs]
            out.append(f"| {NAMES.get(v, v)} | {call} | {len(rs)} | {fmt(statistics.median(w))} | {fmt(min(w))}–{fmt(max(w))} | "
                       f"{fmt(statistics.median(cpu))} | {statistics.median(int(r['maxrss_kb']) for r in rs) / 1024:.1f} MB |")

    # Batch block: one process per tool over the whole corpus (several passes).
    batch = [r for r in rows_of("batch.csv") if good(r)]
    if batch:
        out += ["", "## Batch: one process per tool over the whole corpus", "",
                "Wall time of the whole process, all passes; median over the repetitions (min–max).", "",
                "| tool | runs | documents per run | wall | min–max | pages/s | CPU | peak RSS | time ÷ dox-validate |",
                "|---|---|---|---|---|---|---|---|---|"]
        acc = collections.defaultdict(list)
        for r in batch:
            acc[r["variant"]].append(r)
        ref = statistics.median(float(r["wall_s"]) for r in acc["dox"]) if "dox" in acc else None
        for v in [v for v in KNOWN + EXTRA if v in acc]:
            rs = acc[v]
            w = [float(r["wall_s"]) for r in rs]
            m = statistics.median(w)
            out.append(f"| {NAMES.get(v, v)} | {len(rs)} | {rs[0]['n_files']} | {fmt(m)} | {fmt(min(w))}–{fmt(max(w))} | "
                       f"{int(rs[0]['pages']) / m:,.4g} | {fmt(statistics.median(float(r['user_s']) + float(r['sys_s']) for r in rs))} | "
                       f"{statistics.median(int(r['maxrss_kb']) for r in rs) / 1024:,.0f} MB | "
                       f"{'1 (reference)' if v == 'dox' else (f'{m / ref:.3g}×' if ref else '')} |")

    # Parallel block: the tool's own parallelism (native) and N concurrent one-document processes (pool).
    par = [r for r in rows_of("parallel.csv") if good(r)]
    if par:
        out += ["", "## Parallel: one pass over the corpus with N workers", "",
                "native = `--jobs N` threads in one process; pool = N concurrent one-document processes. Median over the "
                "repetitions. Peak RSS: largest single process, and the sampled sum over all live processes of the run.", "",
                "| mode | tool | N | runs | wall | min–max | pages/s | scaling vs N=1 | CPU | max process RSS | peak tree RSS |",
                "|---|---|---|---|---|---|---|---|---|---|---|"]
        acc = collections.defaultdict(list)
        for r in par:
            acc[(r["block"].replace("parallel-", ""), r["variant"], int(r["workers"]))].append(r)
        one = {(mode, v): statistics.median(float(r["wall_s"]) for r in rs) for (mode, v, n), rs in acc.items() if n == 1}
        for (mode, v, n), rs in sorted(acc.items(), key=lambda kv: (kv[0][0], (KNOWN + EXTRA).index(kv[0][1]), kv[0][2])):
            w = [float(r["wall_s"]) for r in rs]
            m = statistics.median(w)
            tree = [int(r["tree_rss_kb"]) for r in rs if r["tree_rss_kb"]]
            out.append(f"| {mode} | {NAMES.get(v, v)} | {n} | {len(rs)} | {fmt(m)} | {fmt(min(w))}–{fmt(max(w))} | "
                       f"{int(rs[0]['pages']) / m:,.4g} | {one[(mode, v)] / m:.2f}× | "
                       f"{fmt(statistics.median(float(r['user_s']) + float(r['sys_s']) for r in rs))} | "
                       f"{statistics.median(int(r['maxrss_kb']) for r in rs) / 1024:,.0f} MB | "
                       f"{(f'{statistics.median(tree) / 1024:,.0f} MB') if tree else ''} |")

    (res / "tables.md").write_text("\n".join(out) + "\n")
    print("\n".join(out))


if __name__ == "__main__":
    main(Path(sys.argv[1]))
