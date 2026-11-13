#!/usr/bin/env python3
"""Aggregates the raw benchmark results in bench/results/*.csv into summary.json, tables.md and chart.svg.

  python3 -I bench/analyze.py [--results DIR]

Every number in tables.md is computed here from the CSV files written by bench.py; nothing is entered by hand.
Statistics: medians; variability as IQR (25th..75th percentile, inclusive method) or min..max over repetitions.
Runs that failed (tool did not produce a report for every file, non-zero signal, timeout) are excluded from timing
statistics and counted separately.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import statistics as st
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent

VP4 = ["vp-ua1", "vp-ua2", "vp-wt1a", "vp-wt1r"]
LABEL = {
    "dox": "dox-validate (full catalogue)",
    "dox-ua1rules": "dox-validate --rules EQX-UA1-",
    "vp-ua1": "veraPDF 1.30.4 -f ua1",
    "vp-ua2": "veraPDF 1.30.4 -f ua2",
    "vp-wt1a": "veraPDF 1.30.4 -f wt1a",
    "vp-wt1r": "veraPDF 1.30.4 -f wt1r",
    "vp-B": "veraPDF 1.30.4 ua1+ua2+wt1a+wt1r (4 runs, summed)",
    "vp1.30.2-ua1": "veraPDF 1.30.2 -f ua1",
    "cpdf-ua1": "cpdf 2.9.3 -verify PDF/UA-1(matterhorn)",
}


def med(xs):
    xs = [x for x in xs if x is not None]
    return st.median(xs) if xs else None


def iqr(xs):
    xs = sorted(x for x in xs if x is not None)
    if len(xs) < 2:
        return (xs[0], xs[0]) if xs else (None, None)
    q = st.quantiles(xs, n=4, method="inclusive")
    return q[0], q[2]


def gmean(xs):
    xs = [x for x in xs if x and x > 0]
    return math.exp(sum(map(math.log, xs)) / len(xs)) if xs else None


def fnum(x, unit="", nd=None):
    if x is None:
        return "–"
    if nd is None:
        ax = abs(x)
        nd = 0 if ax >= 100 else 1 if ax >= 10 else 2 if ax >= 1 else 3 if ax >= 0.01 else 4
    return f"{x:,.{nd}f}{unit}"


def ftime(s):
    """seconds -> readable (ms below 1 s)."""
    if s is None:
        return "–"
    return f"{fnum(s * 1000)} ms" if s < 1 else f"{fnum(s)} s"


def read_csv(p: Path):
    if not p.exists():
        return []
    with p.open() as f:
        return list(csv.DictReader(f))


def fl(r, k):
    v = r.get(k, "")
    return float(v) if v not in ("", None) else None


def process_ok(r) -> bool:
    """The tool process ran to completion: no timeout, no signal, a readable report with an entry for every file."""
    return (r.get("timeout") in ("0", "") and not r.get("problem")
            and not (r.get("exit", "").lstrip("-").isdigit() and int(r["exit"]) < 0))


def run_ok(r) -> bool:
    """Single-file runs (cold): the process completed and the file was processed."""
    return process_ok(r) and r.get("n_failed") in ("0",)


# ------------------------------------------------------------------------------------------------------------------ cold

def analyze_cold(rows, corpus):
    by = defaultdict(list)          # (variant, item) -> ok rows
    fails = defaultdict(list)       # variant -> [(item, rep, why)]
    for r in rows:
        if run_ok(r):
            by[(r["variant"], r["item"])].append(r)
        else:
            why = "timeout" if r.get("timeout") == "1" else (r.get("problem") or f"exit {r.get('exit')}, {r.get('n_failed')} failed")
            fails[r["variant"]].append((r["item"], r["rep"], why))
    variants = sorted({r["variant"] for r in rows})
    per_file = {}                   # variant -> item -> dict
    for v in variants:
        per_file[v] = {}
        for d in corpus:
            rs = by.get((v, d["id"]), [])
            if not rs:
                continue
            walls = [fl(r, "wall_s") for r in rs]
            m = med(walls)
            per_file[v][d["id"]] = {
                "n": len(rs), "wall": m, "wall_min": min(walls), "wall_max": max(walls),
                "rel_range": (max(walls) - min(walls)) / m if m else None,
                "cpu": med([fl(r, "user_s") + fl(r, "sys_s") for r in rs]),
                "rss_kb": med([fl(r, "maxrss_kb") for r in rs]),
            }
    # composite B: the four veraPDF profile runs per file (sum of per-profile medians; only files where all four exist)
    if all(v in per_file for v in VP4):
        per_file["vp-B"] = {}
        for d in corpus:
            parts = [per_file[v].get(d["id"]) for v in VP4]
            if all(parts):
                per_file["vp-B"][d["id"]] = {
                    "n": min(p["n"] for p in parts), "wall": sum(p["wall"] for p in parts),
                    "wall_min": sum(p["wall_min"] for p in parts), "wall_max": sum(p["wall_max"] for p in parts),
                    "rel_range": None, "cpu": sum(p["cpu"] for p in parts), "rss_kb": max(p["rss_kb"] for p in parts)}
    return per_file, {v: fails[v] for v in fails}


def group_stats(per_file, corpus, ids, ref="dox"):
    """Per variant over the file set ids: medians, IQR across files, throughput from the sum of per-file medians."""
    pages = {d["id"]: d["pages"] for d in corpus}
    out = {}
    for v, pf in per_file.items():
        common = [i for i in ids if i in pf]
        if not common:
            continue
        walls = [pf[i]["wall"] for i in common]
        tot = sum(walls)
        npages = sum(pages[i] for i in common)
        rr = [pf[i]["rel_range"] for i in common if pf[i]["rel_range"] is not None]
        ratio = [pf[i]["wall"] / per_file[ref][i]["wall"] for i in common if i in per_file.get(ref, {}) and per_file[ref][i]["wall"]]
        out[v] = {
            "files": len(common), "pages": npages,
            "median_wall_s": med(walls), "iqr_wall_s": iqr(walls), "max_wall_s": max(walls),
            "sum_wall_s": tot, "docs_per_s": len(common) / tot if tot else None, "pages_per_s": npages / tot if tot else None,
            "median_cpu_s": med([pf[i]["cpu"] for i in common]), "sum_cpu_s": sum(pf[i]["cpu"] for i in common),
            "median_rss_mb": med([pf[i]["rss_kb"] / 1024 for i in common]), "max_rss_mb": max(pf[i]["rss_kb"] / 1024 for i in common),
            "median_rep_rel_range": med(rr),
            "ratio_vs_ref_median": med(ratio), "ratio_vs_ref_gmean": gmean(ratio),
            "ratio_vs_ref_min": min(ratio) if ratio else None, "ratio_vs_ref_max": max(ratio) if ratio else None,
        }
    return out


# ----------------------------------------------------------------------------------------------------------------- batch

def analyze_batch(rows, frows, corpus):
    res = {}
    ok = [r for r in rows if process_ok(r)]
    by = defaultdict(list)
    for r in ok:
        by[r["variant"]].append(r)
    for v, rs in by.items():
        walls = [fl(r, "wall_s") for r in rs]
        n = int(rs[0]["n_files"])
        pg = int(rs[0]["pages"])
        res[v] = {"runs": len(rs), "failed_runs": sum(1 for r in rows if r["variant"] == v and not process_ok(r)),
                  "unprocessed_files_per_run": sorted({int(r["n_failed"]) for r in rs}),
                  "files_per_run": n, "pages_per_run": pg,
                  "median_wall_s": med(walls), "min_wall_s": min(walls), "max_wall_s": max(walls),
                  "docs_per_s": n / med(walls), "docs_per_s_min": n / max(walls), "docs_per_s_max": n / min(walls),
                  "pages_per_s": pg / med(walls),
                  "median_cpu_s": med([fl(r, "user_s") + fl(r, "sys_s") for r in rs]),
                  "median_rss_mb": med([fl(r, "maxrss_kb") / 1024 for r in rs]),
                  "max_rss_mb": max(fl(r, "maxrss_kb") / 1024 for r in rs)}
    # composite B per repetition
    reps = defaultdict(dict)
    for r in ok:
        if r["variant"] in VP4:
            reps[r["rep"]][r["variant"]] = r
    sums = [sum(fl(x, "wall_s") for x in d.values()) for d in reps.values() if len(d) == 4]
    if sums:
        any_r = next(iter(reps.values()))["vp-ua1"]
        n, pg = int(any_r["n_files"]), int(any_r["pages"])
        res["vp-B"] = {"runs": len(sums), "files_per_run": n, "pages_per_run": pg, "median_wall_s": med(sums),
                       "min_wall_s": min(sums), "max_wall_s": max(sums), "docs_per_s": n / med(sums),
                       "docs_per_s_min": n / max(sums), "docs_per_s_max": n / min(sums), "pages_per_s": pg / med(sums),
                       "median_rss_mb": max(res[v]["median_rss_mb"] for v in VP4 if v in res)}
    # tool-reported per-file times: first pass (cold JIT / caches) vs later passes (warm)
    tier = {d["id"]: d["tier"] for d in corpus}
    pages = {d["id"]: d["pages"] for d in corpus}
    t = defaultdict(lambda: defaultdict(list))   # (variant, phase) -> item -> [ms]
    for r in frows:
        if r["status"] not in ("ok", "incomplete") or r["tool_ms"] in ("", None) or r["pass"] == "":
            continue
        phase = "first" if r["pass"] == "0" else "warm"
        t[(r["variant"], phase)][r["item"]].append(float(r["tool_ms"]))
    inproc = {}
    for (v, phase), items in t.items():
        permed = {i: med(x) for i, x in items.items()}
        tot = sum(permed.values()) / 1000
        npg = sum(pages.get(i, 0) for i in permed)
        tiers = defaultdict(list)
        for i, m in permed.items():
            tiers[tier.get(i, "")].append(m)
        inproc.setdefault(v, {})[phase] = {
            "files": len(permed), "sum_s": tot, "docs_per_s": len(permed) / tot if tot else None,
            "pages_per_s": npg / tot if tot else None,
            "median_ms_by_tier": {k: med(x) for k, x in sorted(tiers.items())},
            "per_file_ms": permed}
    return res, inproc


# -------------------------------------------------------------------------------------------------------------- parallel

def analyze_parallel(rows):
    by = defaultdict(list)
    fails = defaultdict(int)
    for r in rows:
        key = (r["block"].replace("parallel-", ""), r["variant"], int(r["workers"]))
        if process_ok(r):
            by[key].append(r)
        else:
            fails[key] += 1
    res = {}
    for (mode, v, w), rs in sorted(by.items()):
        walls = [fl(r, "wall_s") for r in rs]
        n, pg = int(rs[0]["n_files"]), int(rs[0]["pages"])
        tree = [fl(r, "tree_rss_kb") for r in rs if r.get("tree_rss_kb") not in ("", None)]
        res[f"{mode}|{v}|{w}"] = {
            "mode": mode, "variant": v, "workers": w, "runs": len(rs), "failed_runs": fails[(mode, v, w)],
            "unprocessed_files_per_run": sorted({int(r["n_failed"]) for r in rs}),
            "median_wall_s": med(walls), "min_wall_s": min(walls), "max_wall_s": max(walls),
            "docs_per_s": n / med(walls), "docs_per_s_min": n / max(walls), "docs_per_s_max": n / min(walls),
            "pages_per_s": pg / med(walls), "median_cpu_s": med([fl(r, "user_s") + fl(r, "sys_s") for r in rs]),
            "max_proc_rss_mb": max(fl(r, "maxrss_kb") / 1024 for r in rs),
            "peak_tree_rss_mb": max(tree) / 1024 if tree else None,
            "median_peak_tree_rss_mb": med(tree) / 1024 if tree else None}
    for k, d in res.items():
        base = res.get(f"{d['mode']}|{d['variant']}|1")
        d["scaling_vs_1"] = d["docs_per_s"] / base["docs_per_s"] if base else None
    return res


# --------------------------------------------------------------------------------------------------------------- startup

def analyze_startup(rows):
    by = defaultdict(list)
    for r in rows:
        if r.get("exit") in ("0", "1") and r.get("problem", "") == "":
            by[(r["variant"], "version" if r["item"] == "--version" else "smallest")].append(r)
    res = {}
    for (v, item), rs in sorted(by.items()):
        walls = [fl(r, "wall_s") for r in rs]
        res[f"{v}|{item}"] = {"variant": v, "item": item, "runs": len(rs), "median_wall_s": med(walls),
                              "min_wall_s": min(walls), "max_wall_s": max(walls),
                              "median_cpu_s": med([fl(r, "user_s") + fl(r, "sys_s") for r in rs]),
                              "median_rss_mb": med([fl(r, "maxrss_kb") / 1024 for r in rs])}
    return res


def analyze_load(results: Path, blocks_rows: dict):
    snaps = []
    p = results / "load.jsonl"
    if p.exists():
        snaps = [json.loads(l) for l in p.read_text().splitlines() if l.strip()]
    per_block = {}
    for b, rows in blocks_rows.items():
        fc = [fl(r, "foreign_cpu_s") for r in rows if r.get("foreign_cpu_s") not in ("", None)]
        wall = sum(fl(r, "wall_s") for r in rows)
        l1 = [fl(r, "load1") for r in rows if r.get("load1")]
        per_block[b] = {"runs": len(rows), "sum_wall_s": wall, "sum_foreign_cpu_s": sum(fc),
                        "load1_median": med(l1), "load1_max": max(l1) if l1 else None,
                        "load1_min": min(l1) if l1 else None}
    return {"snapshots": snaps, "per_block": per_block}


# ------------------------------------------------------------------------------------------------------------------- svg

CHART_LABEL = {"dox": "dox-validate (full)", "dox-ua1rules": "dox-validate --rules EQX-UA1-",
               "vp-ua1": "veraPDF -f ua1", "vp-B": "veraPDF ua1+ua2+wt1a+wt1r", "cpdf-ua1": "cpdf -verify PDF/UA-1"}


def tick(s):
    return f"{s * 1000:g} ms" if s < 1 else f"{s:g} s"


def chart_svg(groups: dict, order: list[str], tiers: list[str]) -> str:
    """Grouped horizontal bars of the median per-document cold wall time per tier, log scale, every bar labelled."""
    slot = ["s1", "s2", "s3", "s4", "s5"]   # fixed categorical order (validated palette, light and dark steps)
    cls = {v: slot[i] for i, v in enumerate(("dox", "dox-ua1rules", "vp-ua1", "vp-B", "cpdf-ua1"))}
    rows = [(t, v, groups[t][v]["median_wall_s"]) for t in tiers for v in order if t in groups and v in groups[t]]
    vals = [x for *_, x in rows if x]
    if not vals:
        return ""
    lo = 10 ** math.floor(math.log10(min(vals)))
    hi = 10 ** math.ceil(math.log10(max(vals)))
    W, left, right, bar, gap, top = 760, 240, 70, 14, 4, 44
    h = top + len(tiers) * (len(order) * (bar + gap) + 26) + 40
    x = lambda s: left + (math.log10(s) - math.log10(lo)) / (math.log10(hi) - math.log10(lo)) * (W - left - right)
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{h}" viewBox="0 0 {W} {h}" '
           'font-family="system-ui, sans-serif" font-size="12" role="img" '
           'aria-label="Median wall time per document by size tier and tool, one process per file, log scale">',
           "<style>.bg{fill:#fcfcfb}.t1{fill:#1f1f1e}.t2{fill:#5f5e58}.grid{stroke:#e4e3de}"
           ".s1{fill:#2a78d6}.s2{fill:#eb6834}.s3{fill:#1baf7a}.s4{fill:#eda100}.s5{fill:#e87ba4}"
           "@media (prefers-color-scheme: dark){.bg{fill:#1a1a19}.t1{fill:#f0efec}.t2{fill:#c3c2b7}.grid{stroke:#3a3a37}"
           ".s1{fill:#3987e5}.s2{fill:#d95926}.s3{fill:#199e70}.s4{fill:#c98500}.s5{fill:#d55181}}</style>",
           '<rect class="bg" width="100%" height="100%"/>',
           f'<text class="t1" x="8" y="20" font-size="14" font-weight="600">Median wall time per document, one process per '
           f'file (log scale; shorter is faster)</text>']
    e = lo
    while e <= hi * 1.0001:
        out.append(f'<line class="grid" x1="{x(e):.1f}" y1="{top - 8}" x2="{x(e):.1f}" y2="{h - 34}"/>')
        out.append(f'<text class="t2" x="{x(e):.1f}" y="{h - 18}" text-anchor="middle">{tick(e)}</text>')
        e *= 10
    y = top
    for t in tiers:
        n = groups.get(t, {}).get("dox", {}).get("files", 0)
        out.append(f'<text class="t1" x="8" y="{y + 12}" font-weight="600">{t} ({n} documents)</text>')
        y += 20
        for v in order:
            g = groups.get(t, {}).get(v)
            if not g or not g["median_wall_s"]:
                continue
            s = g["median_wall_s"]
            lab = CHART_LABEL.get(v, v) + (f" ({g['files']} docs)" if g["files"] != n else "")
            out.append(f'<text class="t2" x="{left - 8}" y="{y + bar - 3}" text-anchor="end">{lab}</text>')
            out.append(f'<rect class="{cls.get(v, "s1")}" x="{left}" y="{y}" width="{max(2.0, x(s) - left):.1f}" '
                       f'height="{bar}" rx="2"><title>{LABEL.get(v, v)}, {t}: {ftime(s)}</title></rect>')
            out.append(f'<text class="t1" x="{x(s) + 5:.1f}" y="{y + bar - 3}">{ftime(s)}</text>')
            y += bar + gap
        y += 6
    out.append("</svg>")
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------------------------------------------------- tables

def table(headers, rows):
    s = "| " + " | ".join(headers) + " |\n|" + "|".join("---" for _ in headers) + "|\n"
    for r in rows:
        s += "| " + " | ".join(str(c) for c in r) + " |\n"
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", type=Path, default=HERE / "results")
    a = ap.parse_args()
    R = a.results
    env = json.loads((R / "env.json").read_text()) if (R / "env.json").exists() else {}
    corpus = [{**d, "pages": int(d["pages"]), "bytes": int(d["bytes"])} for d in read_csv(R / "corpus.csv")]
    cold, batch, bfiles = read_csv(R / "cold.csv"), read_csv(R / "batch.csv"), read_csv(R / "batch_files.csv")
    par, startup = read_csv(R / "parallel.csv"), read_csv(R / "startup.csv")

    summary = {"env": {k: env.get(k) for k in ("cpu_model", "cpu_set", "dox_commit", "dox_version", "verapdf", "java",
                                                 "corpus_manifest_sha256", "corpus_files", "corpus_pages")}}
    md = []
    tiers = [t for t in ("small", "medium", "large") if any(d["tier"] == t for d in corpus)]
    cx = sorted({d["complexity"] for d in corpus if d["complexity"]})

    # corpus
    crow = []
    for t in tiers:
        for c in cx:
            ds = [d for d in corpus if d["tier"] == t and d["complexity"] == c]
            if ds:
                crow.append([t, c, len(ds), sum(d["pages"] for d in ds), fnum(sum(d["bytes"] for d in ds) / 1e6, " MB", 2),
                             f"{min(d['pages'] for d in ds)}–{max(d['pages'] for d in ds)}"])
    md.append("### Corpus\n\n" + table(["tier", "complexity", "files", "pages", "bytes", "pages per file"], crow))

    # startup
    if startup:
        s = analyze_startup(startup)
        summary["startup"] = s
        md.append("### Start-up (one process; median of repetitions, min–max)\n\n" + table(
            ["tool", "call", "runs", "wall", "min–max", "CPU (user+sys)", "peak RSS"],
            [[LABEL.get(d["variant"], d["variant"]), "--version" if d["item"] == "version" else "smallest corpus file",
              d["runs"], ftime(d["median_wall_s"]), f"{ftime(d['min_wall_s'])}–{ftime(d['max_wall_s'])}",
              ftime(d["median_cpu_s"]), fnum(d["median_rss_mb"], " MB")] for d in s.values()]))

    # cold
    if cold:
        per_file, fails = analyze_cold(cold, corpus)
        groups = {}
        sets = {"all": [d["id"] for d in corpus if d["tier"] in tiers]}
        for t in tiers:
            sets[t] = [d["id"] for d in corpus if d["tier"] == t]
            for c in cx:
                ids = [d["id"] for d in corpus if d["tier"] == t and d["complexity"] == c]
                if ids:
                    sets[f"{t}/{c}"] = ids
        for c in cx:
            sets[f"all/{c}"] = [d["id"] for d in corpus if d["complexity"] == c]
        # Compare on the same documents: those that dox-validate and all four veraPDF profiles processed in every
        # repetition-median (files a tool rejects, e.g. encrypted or malformed input, are listed separately).
        main = [v for v in ("dox", *VP4) if v in per_file]
        common = set.intersection(*(set(per_file[v]) for v in main)) if main else set()
        excluded = sorted(d["id"] for d in corpus if d["id"] not in common)
        for name, ids in sets.items():
            groups[name] = group_stats(per_file, corpus, [i for i in ids if i in common])
        summary["cold"] = {"groups": groups, "common_documents": len(common), "excluded_documents": excluded,
                           "failures": {v: len(x) for v, x in fails.items()},
                           "failure_details": {v: x[:50] for v, x in fails.items()},
                           "per_file_dox_wall_s": {i: d["wall"] for i, d in per_file.get("dox", {}).items()}}
        order = [v for v in ("dox", "dox-ua1rules", "vp-ua1", "vp-B", "cpdf-ua1") if v in per_file]
        rows = []
        for name in ["all", *tiers, *[k for k in sets if "/" in k]]:
            for v in order:
                g = groups[name].get(v)
                if not g:
                    continue
                rows.append([name, LABEL.get(v, v), g["files"], ftime(g["median_wall_s"]),
                             f"{ftime(g['iqr_wall_s'][0])}–{ftime(g['iqr_wall_s'][1])}", ftime(g["max_wall_s"]),
                             fnum(g["docs_per_s"]), fnum(g["pages_per_s"]), fnum(g["median_rss_mb"], " MB"),
                             fnum(g["max_rss_mb"], " MB"),
                             "1 (reference)" if v == "dox" else f"{fnum(g['ratio_vs_ref_median'], '×')} ({fnum(g['ratio_vs_ref_gmean'], '×')})"])
        def cell(name, v, key="median_wall_s"):
            g = groups[name].get(v)
            return ftime(g[key]) if g else "–"

        def ratio(name, v):
            g = groups[name].get(v)
            return f"{fnum(g['ratio_vs_ref_median'], '×')} ({fnum(g['ratio_vs_ref_gmean'], '×')})" if g else "–"

        def cpdf_cell(name, n):
            g = groups[name].get("cpdf-ua1")
            if not g:
                return "–"
            return ftime(g["median_wall_s"]) + ("" if g["files"] == n else f" ({g['files']} of {n} docs)")

        names = ["all", *tiers, *[f"all/{c}" for c in cx], *[k for k in sets if "/" in k and not k.startswith("all/")]]
        crow = []
        for name in names:
            g = groups[name].get("dox")
            if not g:
                continue
            crow.append([name, g["files"], g["pages"], cell(name, "dox"), cell(name, "vp-ua1"), ratio(name, "vp-ua1"),
                         cell(name, "vp-B"), ratio(name, "vp-B"), cell(name, "dox-ua1rules"), cpdf_cell(name, g["files"])])
        md.append("### Cold start, compact: median wall time per document\n\n"
                  "Median over the documents of the group of the per-document median of 5 repetitions. "
                  "time ÷ dox-validate: median of the per-document ratios (geometric mean in brackets); a value of 10× "
                  "means the tool took ten times as long as the full dox-validate run.\n\n" + table(
                      ["group", "docs", "pages", "dox-validate (full)", "veraPDF ua1", "A: ua1 ÷ dox", "veraPDF 4 profiles",
                       "B: 4 profiles ÷ dox", "dox --rules EQX-UA1-", "cpdf (documents it completed)"], crow))
        rrow = []
        for name in ["all", *tiers]:
            row = [name]
            for v in ("dox", "vp-ua1", "vp-B", "cpdf-ua1"):
                g = groups[name].get(v)
                row.append(f"{fnum(g['median_rss_mb'], ' MB')} / {fnum(g['max_rss_mb'], ' MB')}" if g else "–")
            rrow.append(row)
        md.append("### Cold start, peak resident memory per process (median / maximum over the documents)\n\n" + table(
            ["group", "dox-validate (full)", "veraPDF ua1", "veraPDF 4 profiles (max of the 4 runs)", "cpdf"], rrow))
        trow = []
        for name in ["all", *tiers]:
            row = [name]
            for v in ("dox", "vp-ua1", "vp-B", "cpdf-ua1"):
                g = groups[name].get(v)
                row.append(f"{fnum(g['docs_per_s'])} / {fnum(g['pages_per_s'])} / {ftime(g['sum_cpu_s'])}" if g else "–")
            trow.append(row)
        md.append("### Cold start, sequential throughput (documents/s / pages/s / total CPU time)\n\n"
                  "From the sum of the per-document medians, one process at a time. veraPDF uses several threads per "
                  "process (JIT compiler, garbage collector), so its CPU time exceeds its wall time.\n\n" + table(
                      ["group", "dox-validate (full)", "veraPDF ua1", "veraPDF 4 profiles", "cpdf"], trow))
        md.append("### Cold start: one process per document (A, B, supplementary)\n\n"
                  "Median per-document wall time over the documents of the group (each document: median of the repetitions); "
                  "IQR across documents; docs/s and pages/s from the sum of the per-document medians (sequential, one process "
                  "at a time); peak RSS of the tool process (median and maximum over documents); time relative to "
                  "dox-validate: median of the per-document ratios (geometric mean in brackets).\n\n" + table(
                      ["group", "tool", "docs", "median wall", "IQR", "max", "docs/s", "pages/s", "median peak RSS",
                       "max peak RSS", "time ÷ dox-validate"], rows))
        rr = [[LABEL.get(v, v), fnum(100 * (groups["all"][v]["median_rep_rel_range"] or 0), " %", 1)]
              for v in per_file if v in groups["all"] and groups["all"][v]["median_rep_rel_range"] is not None]
        md.append("Repetition variability (cold): median over documents of (max − min) / median of the repetitions\n\n"
                  + table(["tool", "relative range"], rr))
        fr = [[LABEL.get(v, v), len(x), "; ".join(f"{i} ({w}, {n}×)" for (i, w), n in sorted(
            defaultdict(int, {(i, w): sum(1 for j, _, ww in x if j == i and ww == w) for i, _, w in x}).items()))]
              for v, x in sorted(fails.items())]
        md.append(f"Documents in the comparison: {len(common)} (processed by dox-validate and by all four veraPDF "
                  f"profiles); not compared: {', '.join(excluded) or 'none'}\n\n")
        md.append("Cold runs in which the tool did not process the document (excluded from timing)\n\n"
                  + (table(["tool", "runs", "documents: reason"], fr) if fr else "none\n"))
        svg = chart_svg(groups, order, tiers)
        if svg:
            (R / "chart.svg").write_text(svg)

    # batch
    if batch:
        b, inproc = analyze_batch(batch, bfiles, corpus)
        summary["batch"] = b
        summary["batch_inprocess"] = {v: {ph: {k: x for k, x in d.items() if k != "per_file_ms"} for ph, d in p.items()}
                                      for v, p in inproc.items()}
        passes = None
        if batch:
            passes = batch[0]["item"].split("x")[-1]
        order = [v for v in ("dox", "dox-ua1rules", "vp-ua1", "vp-B", "vp1.30.2-ua1") if v in b]
        ref = b.get("dox")
        md.append(f"### Batch: one process per tool over the whole corpus, {passes} passes (sustained, sequential)\n\n"
                  "Wall time of the whole process (start-up, JIT warm-up and every pass included); median over repetitions, "
                  "min–max; docs/s and pages/s over all passes.\n\n" + table(
                      ["tool", "runs", "docs per run", "median wall", "min–max", "docs/s", "pages/s", "CPU", "peak RSS",
                       "time ÷ dox-validate"],
                      [[LABEL.get(v, v), b[v]["runs"], b[v]["files_per_run"], ftime(b[v]["median_wall_s"]),
                        f"{ftime(b[v]['min_wall_s'])}–{ftime(b[v]['max_wall_s'])}", fnum(b[v]["docs_per_s"]),
                        fnum(b[v]["pages_per_s"]), ftime(b[v].get("median_cpu_s")), fnum(b[v]["median_rss_mb"], " MB"),
                        fnum(b[v]["median_wall_s"] / ref["median_wall_s"], "×") if ref else "–"] for v in order]))
        irow = []
        for v in ("dox", "dox-ua1rules", "vp-ua1", "vp-ua2", "vp-wt1a", "vp-wt1r", "vp1.30.2-ua1"):
            for ph in ("first", "warm"):
                d = inproc.get(v, {}).get(ph)
                if d:
                    irow.append([LABEL.get(v, v), "pass 1" if ph == "first" else "passes 2+ (warm)", d["files"],
                                 ftime(d["sum_s"]), fnum(d["docs_per_s"]), fnum(d["pages_per_s"]),
                                 *[fnum(d["median_ms_by_tier"].get(t), " ms") for t in tiers]])
        md.append("Tool-reported per-document processing time inside the batch process (veraPDF: job processingTime; "
                  "dox-validate: timing_ms.total), i.e. without process start-up; per document the median over "
                  "repetitions, then summed.\n\n" + table(
                      ["tool", "phase", "docs", "sum", "docs/s", "pages/s", *[f"median {t}" for t in tiers]], irow))

    # parallel
    if par:
        p = analyze_parallel(par)
        summary["parallel"] = p
        prow = []
        for d in sorted(p.values(), key=lambda d: (d["mode"], d["variant"], d["workers"])):
            prow.append([d["mode"], LABEL.get(d["variant"], d["variant"]), d["workers"], d["runs"],
                         ftime(d["median_wall_s"]), f"{ftime(d['min_wall_s'])}–{ftime(d['max_wall_s'])}",
                         fnum(d["docs_per_s"]), fnum(d["pages_per_s"]), fnum(d["scaling_vs_1"], "×"),
                         ftime(d["median_cpu_s"]), fnum(d["max_proc_rss_mb"], " MB"), fnum(d["peak_tree_rss_mb"], " MB"),
                         d["failed_runs"]])
        md.append("### Parallel throughput over the corpus (one pass)\n\n"
                  "native = the tool's own parallelism (dox-validate `--jobs N` threads in one process; veraPDF "
                  "`--processes N` child JVMs); pool = N concurrent one-document processes started by the harness. "
                  "Peak RSS: largest single process (wait4) and sampled sum over all live processes of the run (20 ms).\n\n"
                  + table(["mode", "tool", "N", "runs", "median wall", "min–max", "docs/s", "pages/s", "scaling vs N=1",
                           "CPU", "max process RSS", "peak tree RSS", "failed runs"], prow))

    abp = read_csv(R / "ab_published.csv")
    if abp:
        tier = {d["id"]: d["tier"] for d in corpus}
        pages = {d["id"]: d["pages"] for d in corpus}
        labels = list(dict.fromkeys(r["binary"] for r in abp))
        per = defaultdict(lambda: defaultdict(list))   # label -> item -> rows
        for r in abp:
            per[r["binary"]][r["item"]].append(r)
        items = sorted(per[labels[0]])
        same_exit = all({r["exit"] for b in labels for r in per[b][i]} .__len__() == 1 for i in items)
        same_find = all(len({r["findings_sha256"] for b in labels for r in per[b][i]}) == 1 for i in items)
        same_proc = all(len({r["processed"] for b in labels for r in per[b][i]}) == 1 for i in items)
        diff_find = [i for i in items if len({r["findings_sha256"] for b in labels for r in per[b][i]}) != 1]
        n_proc = {b: sum(1 for i in items if per[b][i][0]["processed"] == "True") for b in labels}
        wall = {b: {i: med([fl(r, "wall_s") for r in per[b][i]]) for i in items} for b in labels}
        rss = {b: {i: med([fl(r, "maxrss_kb") / 1024 for r in per[b][i]]) for i in items} for b in labels}
        rows, sp = [], {}
        for t in ["all", *tiers]:
            ids = [i for i in items if (t == "all" and tier.get(i) in tiers) or tier.get(i) == t]
            if not ids:
                continue
            mw = {b: med([wall[b][i] for i in ids]) for b in labels}
            mr = {b: med([rss[b][i] for i in ids]) for b in labels}
            xr = {b: max(rss[b][i] for i in ids) for b in labels}
            tot = {b: sum(wall[b][i] for i in ids) for b in labels}
            pg = sum(pages.get(i, 0) for i in ids)
            ratio = med([wall[labels[1]][i] / wall[labels[0]][i] for i in ids])
            sp[t] = {"docs": len(ids), "median_wall_s": mw, "median_rss_mb": mr, "max_rss_mb": xr, "sum_wall_s": tot,
                     "pages_per_s": {b: pg / tot[b] for b in labels}, "median_ratio": ratio}
            rows.append([t, len(ids), *[ftime(mw[b]) for b in labels], fnum(100 * (ratio - 1), " %", 1),
                         *[fnum(pg / tot[b]) for b in labels], *[f"{fnum(mr[b], ' MB')} / {fnum(xr[b], ' MB')}" for b in labels]])
        reps = len(per[labels[0]][items[0]])
        summary["ab_published"] = {"labels": labels, "documents": len(items), "reps": reps, "tiers": sp,
                                   "same_exit_codes": same_exit, "same_processed": same_proc, "processed": n_proc,
                                   "same_findings": same_find, "documents_with_different_findings": diff_find}
        md.append(f"### dox-validate {labels[0]} vs {labels[1]}: {len(items)} corpus files, one process per file, "
                  f"{reps} alternating repetitions\n\n"
                  f"Processed: {', '.join(f'{b} {n_proc[b]}' for b in labels)} of {len(items)}; same exit code for every file: "
                  f"{'yes' if same_exit else 'NO'}; same findings (rule ids and occurrence counts): "
                  f"{'yes' if same_find else 'NO: ' + ', '.join(diff_find)}. Wall change: median of the per-document ratios.\n\n"
                  + table(["group", "docs", *[f"median wall {b}" for b in labels], "wall change",
                           *[f"pages/s {b}" for b in labels], *[f"peak RSS {b} (median / max)" for b in labels]], rows))

    ab = read_csv(R / "ab_dox.csv")
    if ab:
        by = defaultdict(list)
        for r in ab:
            by[(r["item"], r["binary"])].append(r)
        labels = list(dict.fromkeys(r["binary"] for r in ab))
        items = list(dict.fromkeys(r["item"] for r in ab))
        arow, sab = [], {}
        for i in items:
            rs = {b: by[(i, b)] for b in labels}
            med_w = {b: med([fl(r, "wall_s") for r in rs[b]]) for b in labels}
            rng = {b: (min(fl(r, "wall_s") for r in rs[b]), max(fl(r, "wall_s") for r in rs[b])) for b in labels}
            rss = {b: med([fl(r, "maxrss_kb") / 1024 for r in rs[b]]) for b in labels}
            same = len({r["findings_sha256"] for b in labels for r in rs[b]}) == 1
            sab[i] = {"wall_s": med_w, "wall_range_s": rng, "rss_mb": rss, "reports_identical": same,
                      "runs": {b: len(rs[b]) for b in labels}}
            arow.append([i, rs[labels[0]][0]["pages"], len(rs[labels[0]]),
                         *[f"{ftime(med_w[b])} ({ftime(rng[b][0])}–{ftime(rng[b][1])})" for b in labels],
                         fnum(100 * (med_w[labels[1]] / med_w[labels[0]] - 1), " %", 1),
                         *[fnum(rss[b], " MB") for b in labels], "yes" if same else "NO"])
        summary["ab_dox"] = sab
        md.append(f"### dox-validate {labels[0]} vs {labels[1]} (one process per document, alternating order)\n\n" + table(
            ["document", "pages", "runs each", *[f"wall {b} (min–max)" for b in labels], "wall change",
             *[f"peak RSS {b}" for b in labels], "same findings"], arow))

    blocks = {"startup": startup, "cold": cold, "batch": batch, "parallel": par}
    summary["load"] = analyze_load(R, {k: v for k, v in blocks.items() if v})
    lrow = [[b, d["runs"], fnum(d["sum_wall_s"], " s", 0), fnum(d["sum_foreign_cpu_s"], " s", 0),
             fnum(d["sum_foreign_cpu_s"] / (d["sum_wall_s"] * max(1, len(env.get("cpu_set_max_freq_khz") or {}) or 1)) * 100, " %", 1),
             f"{fnum(d['load1_min'], '', 2)} / {fnum(d['load1_median'], '', 2)} / {fnum(d['load1_max'], '', 2)}"]
            for b, d in summary["load"]["per_block"].items()]
    md.append("### Background load during the measurements\n\n"
              "foreign CPU = busy time of the benchmark CPU set (from /proc/stat) minus the CPU time of the measured tool "
              "processes, i.e. other processes and interrupts on those CPUs during the runs.\n\n"
              + table(["block", "runs", "measured wall", "foreign CPU on the set", "share of set capacity",
                       "load1 min / median / max"], lrow))
    (R / "summary.json").write_text(json.dumps(summary, indent=1, default=str) + "\n")
    (R / "tables.md").write_text("<!-- generated by bench/analyze.py from bench/results/*.csv; do not edit -->\n\n" + "\n".join(md))
    print((R / "tables.md").read_text())


if __name__ == "__main__":
    main()
