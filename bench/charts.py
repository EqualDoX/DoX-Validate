#!/usr/bin/env python3
"""Benchmark charts (SVG) from the stored measurements: validation time and peak memory by page count, and a summary.

    python3 -I bench/charts.py [--out docs/charts] [--corpus-run DIR] [--extension-run DIR]

Inputs (never modified): the cold blocks of two benchmark runs, one process per document and tool:
  bench/results/optimised-2026-11-14/   the frozen test corpus (157 files)
  bench/results/extension-2026-11-15/   61 public documents of bench/extension/manifest.json, plus 14 anchor documents
                                        of the frozen corpus measured again in the same run

Method (also in docs/BENCHMARKS.md):
  - A run of a tool on a document counts if the tool processed the document (bench.py's check). Per document and tool:
    the median wall time and the largest peak RSS over the repetitions. A document is used only if every plotted tool
    processed it in every repetition of its run, so all tools are compared on the same documents.
  - Frozen-corpus documents are taken from the first run, extension documents from the second. The anchors are not
    plotted; they compare the two runs (ratio of the second run to the first, per document and tool).
  - Page-count intervals: 1, 2-4, 5-9, 10-19, 20-49, 50-99, 100-199, 200-499, 500-999, 1000+. A point is the median
    over the interval's documents, at the interval's median page count; the band is the interquartile range over
    those documents (inclusive quartiles). An interval with fewer than MIN_DOCS documents is not drawn, and a line
    does not cross it.
  - Summary: size tiers as in tests/corpus/build_corpus.py (small <= 10 pages, medium 11-299, large >= 300); time is
    the median over the tier's documents; throughput = pages / summed per-document median times; peak RSS median and
    maximum over all documents.
Outputs (deterministic: the same inputs give the same bytes): <name>-light.svg and <name>-dark.svg for
validation-time, peak-memory and summary, and chart-data.json with every plotted number, the correlations of time and
memory with page count and file size, the run-to-run comparison of the anchors, and the documents left out.
Standard library only.
"""
from __future__ import annotations

import argparse
import collections
import csv
import json
import math
import statistics
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUNS = [("corpus", ROOT / "bench/results/optimised-2026-11-14"), ("extension", ROOT / "bench/results/extension-2026-11-15")]
BINS = [(1, 1), (2, 4), (5, 9), (10, 19), (20, 49), (50, 99), (100, 199), (200, 499), (500, 999), (1000, None)]
MIN_DOCS = 5
TIERS = [("small", "Small PDFs (≤ 10 pages)", 1, 10), ("medium", "Medium PDFs (11–299 pages)", 11, 299),
         ("large", "Large PDFs (≥ 300 pages)", 300, None)]

# variant, short label (direct labels, summary rows), legend label, marker, line width, dash, light colour, dark colour.
# Colours: DoX-Validate in the EqualDoX brand green (website --color-primary); the others are reference-palette hues in
# an order that passes the palette checks in both themes (adjacent pairs; all pairs among the three lowest lines).
TOOLS = [
    ("dox", "DoX-Validate", "DoX-Validate 0.2.0, static build", "circle", 3.0, None, "#16a34a", "#16a34a"),
    ("dox-shared", "DoX-Validate (default build)", "DoX-Validate 0.2.0, default build (shared libqpdf)", "ring", 2.0,
     "6 4", "#16a34a", "#16a34a"),
    ("vellum-ua1", "VellumPdf", "VellumPdf 2.3.3 (-p ua1)", "square", 2.0, None, "#6250d6", "#9085e9"),
    ("cpdf-ua1", "cpdf", "cpdf 2.9.3 (PDF/UA-1 Matterhorn subset)", "diamond", 2.0, None, "#e87ba4", "#d55181"),
    ("outloud-conf", "outloud", "outloud 0.2.1 (conformance layer)", "triangle", 2.0, None, "#2a78d6", "#3987e5"),
    ("vp-ua1", "veraPDF", "veraPDF 1.30.4 (-f ua1)", "triangle-down", 2.0, None, "#e34948", "#e66767"),
]
THEMES = {
    "light": {"bg": "#ffffff", "text": "#1f2328", "text2": "#59636e", "grid": "#e8ebef", "axis": "#b7bec7", "band": 0.10},
    "dark": {"bg": "#0d1117", "text": "#f0f6fc", "text2": "#9da7b3", "grid": "#21262d", "axis": "#4a525c", "band": 0.16},
}
FONT = "-apple-system, BlinkMacSystemFont, 'Segoe UI', 'Noto Sans', Helvetica, Arial, sans-serif"


# --------------------------------------------------------------------------------------------------------------- data

def load_run(path: Path):
    corpus = {r["id"]: r for r in csv.DictReader(open(path / "corpus.csv"))}
    runs = collections.defaultdict(lambda: collections.defaultdict(list))
    fails = collections.defaultdict(collections.Counter)
    reps = 0
    for r in csv.DictReader(open(path / "cold.csv")):
        reps = max(reps, int(r["rep"]) + 1)
        ok = r["n_failed"] in ("0", "") and r["timeout"] in ("False", "0", "") and r["n_ok"] != "0"
        if ok:
            runs[r["item"]][r["variant"]].append((float(r["wall_s"]), int(r["maxrss_kb"] or 0) / 1024))
        else:
            why = "timeout" if r["timeout"] in ("1", "True") else f"exit {r['exit']}"
            fails[(r["item"], r["variant"])][why] += 1
    return corpus, runs, fails, reps


def quartiles(xs):
    if len(xs) == 1:
        return xs[0], xs[0]
    q = statistics.quantiles(xs, n=4, method="inclusive")
    return q[0], q[2]


def ranks(x):
    order = sorted(range(len(x)), key=lambda i: x[i])
    r = [0.0] * len(x)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and x[order[j + 1]] == x[order[i]]:
            j += 1
        for k in range(i, j + 1):
            r[order[k]] = (i + j) / 2 + 1
        i = j + 1
    return r


def pearson(a, b):
    ma, mb = statistics.fmean(a), statistics.fmean(b)
    num = sum((x - ma) * (y - mb) for x, y in zip(a, b))
    den = math.sqrt(sum((x - ma) ** 2 for x in a) * sum((y - mb) ** 2 for y in b))
    return num / den


def sig(x: float, n: int = 4) -> float:
    return float(f"{x:.{n}g}")


def collect():
    tools = [t[0] for t in TOOLS]
    docs, excluded, anchor_rows = [], [], []
    loaded = {name: load_run(path) for name, path in RUNS}
    for name, (corpus, runs, fails, reps) in loaded.items():
        for doc_id, c in sorted(corpus.items()):
            if name == "extension" and c["group"] != "extension":
                continue
            pages = int(c["pages"] or 0)
            have = runs.get(doc_id, {})
            missing = [t for t in tools if len(have.get(t, [])) != reps]
            if missing or pages == 0:
                excluded.append({"id": doc_id, "run": name, "pages": pages, "not_processed_by": {
                    t: dict(sorted(fails.get((doc_id, t), {}).items())) or "no complete set of runs" for t in missing}})
                continue
            docs.append({"id": doc_id, "run": name, "pages": pages, "bytes": int(c["bytes"]),
                         "tagged": c["complexity"] == "complex", "generated": c["origin"] == "generated",
                         "tier": next(k for k, _, lo, hi in TIERS if pages >= lo and (hi is None or pages <= hi)),
                         **{t: (statistics.median(w for w, _ in have[t]), max(m for _, m in have[t])) for t in tools}})
    # anchors: the same frozen-corpus documents in both runs
    _, a_runs, _, a_reps = loaded["corpus"]
    e_corpus, e_runs, _, e_reps = loaded["extension"]
    for doc_id, c in sorted(e_corpus.items()):
        if c["group"] != "anchor":
            continue
        row = {"id": doc_id, "pages": int(c["pages"])}
        for t in tools:
            a, b = a_runs.get(doc_id, {}).get(t, []), e_runs.get(doc_id, {}).get(t, [])
            if len(a) == a_reps and len(b) == e_reps:
                row[t] = (statistics.median(w for w, _ in b) / statistics.median(w for w, _ in a),
                          max(m for _, m in b) / max(m for _, m in a))
        anchor_rows.append(row)
    return docs, excluded, anchor_rows, {name: v[3] for name, v in loaded.items()}


def analyse(docs, excluded, anchor_rows, reps):
    tools = [t[0] for t in TOOLS]
    out = {"method": {
        "runs": {name: str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else path.name for name, path in RUNS},
        "per_document": "median wall time and largest peak RSS over the repetitions; documents processed by every "
                        "tool in every repetition",
        "interval_statistic": "median over the documents of the interval; band = inclusive quartiles",
        "min_documents_per_interval": MIN_DOCS, "rss_unit": "MB = 2^20 bytes (ru_maxrss / 1024)",
        "repetitions": min(reps.values()), "repetitions_by_run": reps},
        "documents": len(docs), "documents_by_run": dict(collections.Counter(d["run"] for d in docs)),
        "intervals": [], "summary": {}, "correlation": {}, "anchors": {}, "excluded": excluded}
    for lo, hi in BINS:
        ds = [d for d in docs if d["pages"] >= lo and (hi is None or d["pages"] <= hi)]
        e = {"pages": f"{lo}" if lo == hi else f"{lo}-{hi}" if hi else f"{lo}+", "lo": lo, "hi": hi, "n": len(ds),
             "n_by_run": dict(sorted(collections.Counter(d["run"] for d in ds).items())),
             "tagged": sum(d["tagged"] for d in ds), "generated": sum(d["generated"] for d in ds),
             "median_pages": statistics.median(d["pages"] for d in ds) if ds else None,
             "time_s": {}, "rss_mb": {}}
        for t in tools:
            if not ds:
                continue
            w = sorted(d[t][0] for d in ds)
            m = sorted(d[t][1] for d in ds)
            e["time_s"][t] = {"median": sig(statistics.median(w)), "q1": sig(quartiles(w)[0]), "q3": sig(quartiles(w)[1])}
            e["rss_mb"][t] = {"median": sig(statistics.median(m)), "q1": sig(quartiles(m)[0]), "q3": sig(quartiles(m)[1])}
        out["intervals"].append(e)
    for key, label, lo, hi in TIERS:
        ds = [d for d in docs if d["tier"] == key]
        out["summary"][key] = {"label": label, "n": len(ds), "pages": sum(d["pages"] for d in ds),
                               "median_time_s": {t: sig(statistics.median(d[t][0] for d in ds)) for t in tools}}
    pages = sum(d["pages"] for d in docs)
    out["summary"]["all"] = {
        "n": len(docs), "pages": pages,
        "throughput_pages_per_s": {t: sig(pages / sum(d[t][0] for d in docs)) for t in tools},
        "median_rss_mb": {t: sig(statistics.median(d[t][1] for d in docs)) for t in tools},
        "max_rss_mb": {t: sig(max(d[t][1] for d in docs)) for t in tools},
        "max_rss_document": {t: max(docs, key=lambda d: d[t][1])["id"] for t in tools}}
    # Aggregate versus per-document claims: each DoX-Validate build against the four other tools, per document and per
    # interval, for time and memory. Every case in which another tool is lower is listed.
    others = [t for t in tools if not t.startswith("dox")]
    out["comparison"] = {}
    for build in ("dox", "dox-shared"):
        c = {}
        for key, idx, metric in (("time", 0, "time_s"), ("rss", 1, "rss_mb")):
            cands = [build] + others
            c[f"lowest_{key}_documents"] = dict(sorted(collections.Counter(
                min(cands, key=lambda t: d[t][idx]) for d in docs).items()))
            c[f"documents_another_tool_lower_{key}"] = [
                {"id": d["id"], "pages": d["pages"], "tool": min(others, key=lambda t: d[t][idx]),
                 "value": sig(min(d[t][idx] for t in others)), "dox": sig(d[build][idx])}
                for d in docs if min(d[t][idx] for t in others) < d[build][idx]]
            c[f"intervals_another_tool_lower_{key}"] = [
                {"pages": e["pages"], "tool": t, "value": e[metric][t]["median"], "dox": e[metric][build]["median"]}
                for e in out["intervals"] if e["n"] >= MIN_DOCS for t in others
                if e[metric][t]["median"] < e[metric][build]["median"]]
        out["comparison"][build] = c
    # correlations of time and memory with page count and with file size
    for label, sel in (("all", docs), ("10+ pages", [d for d in docs if d["pages"] >= 10])):
        p = [d["pages"] for d in sel]
        b = [d["bytes"] for d in sel]
        c = {"n": len(sel), "pages_vs_bytes_spearman": round(pearson(ranks(p), ranks(b)), 3), "tools": {}}
        for t in tools:
            row = {}
            for metric, idx in (("time", 0), ("rss", 1)):
                y = [d[t][idx] for d in sel]
                for xname, x in (("pages", p), ("bytes", b)):
                    row[f"{metric}_vs_{xname}"] = {"spearman": round(pearson(ranks(x), ranks(y)), 3),
                                                   "pearson_log_log": round(pearson([math.log(v) for v in x],
                                                                                    [math.log(v) for v in y]), 3)}
            c["tools"][t] = row
        out["correlation"][label] = c
    for t in tools:
        rt = [r[t][0] for r in anchor_rows if t in r]
        rm = [r[t][1] for r in anchor_rows if t in r]
        if rt:
            out["anchors"][t] = {"n": len(rt), "time_ratio_median": round(statistics.median(rt), 3),
                                 "time_ratio_min": round(min(rt), 3), "time_ratio_max": round(max(rt), 3),
                                 "rss_ratio_median": round(statistics.median(rm), 3),
                                 "rss_ratio_min": round(min(rm), 3), "rss_ratio_max": round(max(rm), 3)}
    out["per_document"] = [{"id": d["id"], "run": d["run"], "pages": d["pages"], "bytes": d["bytes"], "tagged": d["tagged"],
                            "time_s": {t: sig(d[t][0]) for t in tools}, "rss_mb": {t: sig(d[t][1]) for t in tools}}
                           for d in sorted(docs, key=lambda d: (d["pages"], d["id"]))]
    out["anchor_documents"] = [{"id": r["id"], "pages": r["pages"],
                                **{t: [round(r[t][0], 3), round(r[t][1], 3)] for t in tools if t in r}}
                               for r in anchor_rows]
    return out


# ---------------------------------------------------------------------------------------------------------------- svg

def f1(x: float) -> str:
    s = f"{x:.1f}"
    return s[:-2] if s.endswith(".0") else s


def esc(s: str) -> str:
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


class Svg:
    def __init__(self, w, h, theme, title, desc):
        self.w, self.h, self.t = w, h, THEMES[theme]
        self.parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}" '
                      f'role="img" aria-labelledby="t d" font-family="{FONT}">',
                      f"<title id=\"t\">{esc(title)}</title>", f"<desc id=\"d\">{esc(desc)}</desc>",
                      f'<rect width="{w}" height="{h}" fill="{self.t["bg"]}"/>']

    def add(self, s):
        self.parts.append(s)

    def text(self, x, y, s, size: float = 12, color="text", anchor="start", weight=None, extra=""):
        w = f' font-weight="{weight}"' if weight else ""
        self.add(f'<text x="{f1(x)}" y="{f1(y)}" font-size="{size}" fill="{self.t[color]}" text-anchor="{anchor}"{w}'
                 f'{extra}>{esc(s)}</text>')

    def line(self, x1, y1, x2, y2, color, width=1):
        self.add(f'<line x1="{f1(x1)}" y1="{f1(y1)}" x2="{f1(x2)}" y2="{f1(y2)}" stroke="{color}" stroke-width="{width}"/>')

    def done(self):
        return "\n".join(self.parts + ["</svg>"]) + "\n"


def marker(svg, shape, x, y, color, r=4.5):
    bg = svg.t["bg"]
    ring = f'stroke="{bg}" stroke-width="2"'
    if shape == "circle":
        svg.add(f'<circle cx="{f1(x)}" cy="{f1(y)}" r="{f1(r + 0.5)}" fill="{color}" {ring}/>')
    elif shape == "ring":
        svg.add(f'<circle cx="{f1(x)}" cy="{f1(y)}" r="{f1(r)}" fill="{bg}" stroke="{color}" stroke-width="2"/>')
    elif shape == "square":
        s = r * 0.9
        svg.add(f'<rect x="{f1(x - s)}" y="{f1(y - s)}" width="{f1(2 * s)}" height="{f1(2 * s)}" fill="{color}" {ring}/>')
    elif shape == "diamond":
        s = r * 1.25
        svg.add(f'<path d="M{f1(x)} {f1(y - s)}L{f1(x + s)} {f1(y)}L{f1(x)} {f1(y + s)}L{f1(x - s)} {f1(y)}Z" '
                f'fill="{color}" {ring}/>')
    else:
        s = r * 1.25
        d = 1 if shape == "triangle" else -1
        svg.add(f'<path d="M{f1(x)} {f1(y - d * s)}L{f1(x + s)} {f1(y + d * s * 0.8)}L{f1(x - s)} '
                f'{f1(y + d * s * 0.8)}Z" fill="{color}" {ring}/>')


def log_ticks(lo, hi):
    return [10 ** k for k in range(math.floor(math.log10(lo)), math.ceil(math.log10(hi)) + 1)]


def fmt_time(s: float) -> str:
    if s < 1:
        ms = s * 1000
        return f"{ms:.1f} ms" if ms < 10 else f"{ms:,.0f} ms"
    return f"{s:.1f} s" if s < 10 else f"{s:,.0f} s"


def fmt_mb(x: float) -> str:
    return f"{x:.1f} MB" if x < 10 else f"{x:,.0f} MB"


def tick_time(s: float) -> str:
    return f"{s * 1000:g} ms" if s < 1 else f"{s:g} s"


def tick_mb(x: float) -> str:
    return f"{x:,.0f} MB" if x >= 1 else f"{x:g} MB"


def line_chart(data, metric, theme, title, subtitle, ylabel, tickfmt, foot):
    W, H = 960, 700
    L, R, T, B = 92, W - 196, 176, H - 158
    svg = Svg(W, H, theme, title, f"{subtitle} {foot[0]}")
    iv = [e for e in data["intervals"] if e["n"] >= MIN_DOCS]
    vals = [v for e in iv for t in e[metric].values() for v in (t["q1"], t["q3"], t["median"])]
    ylo, yhi = log_ticks(min(vals), max(vals))[0], log_ticks(min(vals), max(vals))[-1]
    xlo, xhi = 0.75, 3000.0

    def X(p):
        return L + (math.log10(p) - math.log10(xlo)) / (math.log10(xhi) - math.log10(xlo)) * (R - L)

    def Y(v):
        return B - (math.log10(v) - math.log10(ylo)) / (math.log10(yhi) - math.log10(ylo)) * (B - T)

    svg.text(24, 38, title, 22, weight="600")
    svg.text(24, 64, subtitle, 14, "text2")
    # legend: three rows of two
    for i, (tid, short, legend, shape, width, dash, cl, cd) in enumerate(TOOLS):
        color = cl if theme == "light" else cd
        lx, ly = 24 + (i % 2) * 440, 98 + (i // 2) * 24
        da = f' stroke-dasharray="{dash}"' if dash else ""
        svg.add(f'<line x1="{lx}" y1="{ly}" x2="{lx + 26}" y2="{ly}" stroke="{color}" stroke-width="{width}"'
                f' stroke-linecap="round"{da}/>')
        marker(svg, shape, lx + 13, ly, color, 4)
        svg.text(lx + 34, ly + 5, legend, 13.5, weight="600" if tid == "dox" else None)
    # grid and axes
    for v in log_ticks(ylo, yhi):
        svg.line(L, Y(v), R, Y(v), svg.t["grid"])
        svg.text(L - 8, Y(v) + 4.5, tickfmt(v), 12.5, "text2", "end")
    for p in (1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000):
        svg.line(X(p), T, X(p), B, svg.t["grid"])
        svg.text(X(p), B + 20, f"{p:,}", 12.5, "text2", "middle")
    svg.line(L, B, R, B, svg.t["axis"])
    svg.line(L, T, L, B, svg.t["axis"])
    svg.text((L + R) / 2, B + 42, "Pages per document (log scale)", 13.5, "text2", "middle")
    svg.text(20, (T + B) / 2, ylabel, 13.5, "text2", "middle", extra=f' transform="rotate(-90 20 {f1((T + B) / 2)})"')
    # documents per interval
    svg.text(L - 8, B + 64, "documents", 12, "text2", "end")
    for e in iv:
        svg.text(X(e["median_pages"]), B + 64, str(e["n"]), 12, "text2", "middle")
    # bands, then lines, then markers (DoX-Validate last, on top)
    order = [t for t in TOOLS if t[0] != "dox"] + [t for t in TOOLS if t[0] == "dox"]
    segs = []   # runs of consecutive drawn intervals
    cur = []
    for e in data["intervals"]:
        if e["n"] >= MIN_DOCS:
            cur.append(e)
        elif cur:
            segs.append(cur)
            cur = []
    if cur:
        segs.append(cur)
    for tid, short, legend, shape, width, dash, cl, cd in order:
        color = cl if theme == "light" else cd
        if tid == "dox-shared":
            continue   # no band for the second build of the same tool: it would cover DoX-Validate's band
        for s in segs:
            up = " ".join(f"{f1(X(e['median_pages']))},{f1(Y(e[metric][tid]['q3']))}" for e in s)
            dn = " ".join(f"{f1(X(e['median_pages']))},{f1(Y(e[metric][tid]['q1']))}" for e in reversed(s))
            svg.add(f'<polygon points="{up} {dn}" fill="{color}" fill-opacity="{svg.t["band"]}" stroke="none"/>')
    for tid, short, legend, shape, width, dash, cl, cd in order:
        color = cl if theme == "light" else cd
        da = f' stroke-dasharray="{dash}"' if dash else ""
        for s in segs:
            pts = " ".join(f"{f1(X(e['median_pages']))},{f1(Y(e[metric][tid]['median']))}" for e in s)
            svg.add(f'<polyline points="{pts}" fill="none" stroke="{color}" stroke-width="{width}" '
                    f'stroke-linejoin="round" stroke-linecap="round"{da}/>')
    for tid, short, legend, shape, width, dash, cl, cd in order:
        color = cl if theme == "light" else cd
        for e in iv:
            marker(svg, shape, X(e["median_pages"]), Y(e[metric][tid]["median"]), color, 5 if tid == "dox" else 4)
    # direct labels at the line ends, spread apart with leader lines where they would collide
    last = iv[-1]
    ends = sorted(((Y(last[metric][t[0]]["median"]), t) for t in TOOLS), key=lambda x: x[0])
    ys = [y for y, _ in ends]
    gap = 16
    for i in range(1, len(ys)):
        ys[i] = max(ys[i], ys[i - 1] + gap)
    over = ys[-1] - (B - 4)
    if over > 0:
        ys = [y - over for y in ys]
        for i in range(len(ys) - 2, -1, -1):
            ys[i] = min(ys[i], ys[i + 1] - gap)
    x0 = X(last["median_pages"])
    for (y, t), ly in zip(ends, ys):
        color = t[6] if theme == "light" else t[7]
        if abs(ly - y) > 2:
            svg.add(f'<path d="M{f1(x0 + 8)} {f1(y)}L{f1(x0 + 16)} {f1(ly)}L{f1(x0 + 20)} {f1(ly)}" fill="none" '
                    f'stroke="{svg.t["axis"]}" stroke-width="1"/>')
        else:
            svg.line(x0 + 8, y, x0 + 20, ly, svg.t["axis"])
        svg.text(x0 + 24, ly + 4.5, t[1], 13, weight="600" if t[0] == "dox" else None)
        svg.add(f'<circle cx="{f1(x0 + 20)}" cy="{f1(ly)}" r="2" fill="{color}"/>')
    for i, s in enumerate(foot):
        svg.text(24, H - 17 * len(foot) - 2 + i * 17, s, 12, "text2")
    return svg.done()


def summary_chart(data, theme):
    S = data["summary"]
    W, H = 1000, 630
    svg = Svg(W, H, theme, "Benchmark Summary",
              "Median validation time per size tier, throughput, and median and maximum peak memory, for each tool.")
    svg.text(24, 38, "Benchmark Summary", 22, weight="600")
    svg.text(24, 60, f"Same {S['all']['n']} documents for every tool, one process per document; "
                     "time and memory: lower is better, throughput: higher is better.", 14, "text2")
    panels = [
        (S["small"]["label"], f"median time, {S['small']['n']} documents", S["small"]["median_time_s"], fmt_time, tick_time),
        (S["medium"]["label"], f"median time, {S['medium']['n']} documents", S["medium"]["median_time_s"], fmt_time,
         tick_time),
        (S["large"]["label"], f"median time, {S['large']['n']} documents", S["large"]["median_time_s"], fmt_time, tick_time),
        ("Throughput", f"pages per second, {S['all']['pages']:,} pages", S["all"]["throughput_pages_per_s"],
         lambda v: f"{v:,.0f}", lambda v: f"{v:,.0f}"),
        ("Median peak memory", "resident set size (RSS)", S["all"]["median_rss_mb"], fmt_mb, tick_mb),
        ("Maximum peak memory", "largest RSS over all documents", S["all"]["max_rss_mb"], fmt_mb, tick_mb),
    ]
    names_w, pw, gap_x = 214, 238, 18
    row_h, top0 = 25, 106
    ph = 70 + len(TOOLS) * row_h
    for k, (ptitle, psub, values, vf, tf) in enumerate(panels):
        col, row = k % 3, k // 3
        x0 = names_w + col * (pw + gap_x)
        y0 = top0 + row * (ph + 30)
        svg.text(x0, y0, ptitle, 14.5, weight="600")
        svg.text(x0, y0 + 18, psub, 12.5, "text2")
        lo, hi = min(values.values()), max(values.values())
        ticks = log_ticks(lo, hi)
        a, b = ticks[0], ticks[-1]
        if len(ticks) < 3:
            a, b = a / 10 if lo / a < 2 else a, b
        px0, px1 = x0, x0 + pw - 70

        def X(v, a=a, b=b, px0=px0, px1=px1):
            return px0 + (math.log10(v) - math.log10(a)) / (math.log10(b) - math.log10(a)) * (px1 - px0)

        ry0 = y0 + 38
        ry1 = ry0 + (len(TOOLS) - 1) * row_h
        for tv in log_ticks(a, b):
            svg.line(X(tv), ry0 - 8, X(tv), ry1 + 8, svg.t["grid"])
            svg.text(X(tv), ry1 + 26, tf(tv), 11.5, "text2", "middle")
        for i, (tid, short, legend, shape, width, dash, cl, cd) in enumerate(TOOLS):
            color = cl if theme == "light" else cd
            y = ry0 + i * row_h
            if col == 0:
                svg.text(24, y + 4.5, short, 13.5, weight="600" if tid == "dox" else None)
            v = values[tid]
            svg.line(px0, y, X(v), y, svg.t["grid"], 1)
            marker(svg, shape, X(v), y, color, 4.5)
            svg.text(X(v) + 10, y + 4.5, vf(v), 12.5, weight="600" if tid == "dox" else None)
    for i, s in enumerate(["Log scales; size tiers as in the test corpus. Tools check different rule sets: DoX-Validate its "
                           "full catalogue (PDF/UA-1, PDF/UA-2, WTPDF,",
                           "PAC-equivalent and WCAG checks), the others PDF/UA-1. Method and data: docs/BENCHMARKS.md; "
                           "regenerated with bench/charts.py."]):
        svg.text(24, H - 36 + i * 17, s, 12, "text2")
    return svg.done()


def tables_md(data) -> str:
    """The charts as tables (their text equivalent), and the checks behind them."""
    names = {t[0]: t[1] for t in TOOLS}
    tools = [t[0] for t in TOOLS]
    head = "| pages | documents | " + " | ".join(names[t] for t in tools) + " |"
    rule = "|---|---|" + "---|" * len(tools)
    out_c = ["## Documents per interval", "",
             "| pages | documents | frozen test corpus | added public documents | tagged | generated test files | median pages |",
             "|---|---|---|---|---|---|---|"]
    for e in data["intervals"]:
        out_c.append(f"| {e['pages']} | {e['n']} | {e['n_by_run'].get('corpus', 0)} | {e['n_by_run'].get('extension', 0)} | "
                     f"{e['tagged']} | {e['generated']} | {e['median_pages']:g} |")
    out = ["# Benchmark chart data", "",
           "Generated by `bench/charts.py` from the stored measurements; do not edit. The charts in this directory show "
           "these values. Method: [docs/BENCHMARKS.md](../BENCHMARKS.md#charts-validation-time-and-memory-by-page-count). "
           "Every document's values are in chart-data.json (per_document).", ""]
    out += out_c + [""]
    for metric, title, fmt in (("time_s", "Validation time per document: median (interquartile range)", fmt_time),
                               ("rss_mb", "Peak resident memory per document: median (interquartile range)", fmt_mb)):
        out += [f"## {title}", "", head, rule]
        for e in data["intervals"]:
            if not e["n"]:
                continue
            cells = [f"{fmt(e[metric][t]['median'])} ({fmt(e[metric][t]['q1'])}–{fmt(e[metric][t]['q3'])})" for t in tools]
            drawn = "" if e["n"] >= MIN_DOCS else " (not drawn)"
            out.append(f"| {e['pages']}{drawn} | {e['n']} | " + " | ".join(cells) + " |")
        out.append("")
    S = data["summary"]
    out += ["## Summary", "", "| measure | documents | " + " | ".join(names[t] for t in tools) + " |",
            "|---|---|" + "---|" * len(tools)]
    for key in ("small", "medium", "large"):
        out.append(f"| {S[key]['label']}: median time | {S[key]['n']} | "
                   + " | ".join(fmt_time(S[key]["median_time_s"][t]) for t in tools) + " |")
    A = S["all"]
    out.append(f"| throughput, pages/s ({A['pages']:,} pages) | {A['n']} | "
               + " | ".join(f"{A['throughput_pages_per_s'][t]:,.0f}" for t in tools) + " |")
    out.append(f"| median peak RSS | {A['n']} | " + " | ".join(fmt_mb(A["median_rss_mb"][t]) for t in tools) + " |")
    out.append(f"| maximum peak RSS | {A['n']} | " + " | ".join(
        f"{fmt_mb(A['max_rss_mb'][t])} ({A['max_rss_document'][t]})" for t in tools) + " |")
    others = [t for t in tools if not t.startswith("dox")]
    out += ["", "## Per document and per interval: where another tool is lower", "",
            "Each DoX-Validate build against VellumPdf, cpdf, outloud and veraPDF (the two builds are not compared with "
            "each other). Documents: how many of them each tool had the lowest value on.", "",
            "| build | measure | that build | " + " | ".join(names[t] for t in others) + " |",
            "|---|---|---|" + "---|" * len(others)]
    for build in ("dox", "dox-shared"):
        c = data["comparison"][build]
        for key, label in (("time", "lowest median time"), ("rss", "lowest peak RSS")):
            w = c[f"lowest_{key}_documents"]
            out.append(f"| {names[build]} | {label} | {w.get(build, 0)} | " + " | ".join(str(w.get(t, 0)) for t in others) + " |")
    for build in ("dox", "dox-shared"):
        c = data["comparison"][build]
        for key, label, fmt in (("time", "time", fmt_time), ("rss", "peak RSS", fmt_mb)):
            iv = c[f"intervals_another_tool_lower_{key}"]
            rows = c[f"documents_another_tool_lower_{key}"]
            out += ["", f"{names[build]}, {label}: intervals with a lower median of another tool: "
                        + (", ".join(f"{r['pages']} pages ({names[r['tool']]} {fmt(r['value'])} vs {fmt(r['dox'])})"
                                     for r in iv) or "none")
                    + f"; documents with a lower value of another tool: {len(rows)}", ""]
            if rows and len(rows) <= 40:
                out += ["| document | pages | lowest other tool | its value | DoX-Validate |", "|---|---|---|---|---|"]
                out += [f"| {r['id']} | {r['pages']} | {names[r['tool']]} | {fmt(r['value'])} | {fmt(r['dox'])} |"
                        for r in rows]
            elif rows:
                by_tool = collections.Counter(names[r["tool"]] for r in rows)
                pages = sorted(r["pages"] for r in rows)
                out.append(f"({len(rows)} documents, {pages[0]}–{pages[-1]} pages; lowest other tool: "
                           + ", ".join(f"{k} {v}" for k, v in sorted(by_tool.items())) + "; listed in chart-data.json)")
    out += ["", "## Correlation with page count and file size", "",
            "Spearman rank correlation / Pearson correlation of the logarithms, per tool, of the per-document median time "
            "and peak RSS with the page count and the file size.", ""]
    for label, c in data["correlation"].items():
        out += [f"Documents: {label} (n = {c['n']}; page count vs file size, Spearman {c['pages_vs_bytes_spearman']})", "",
                "| tool | time ~ pages | time ~ bytes | RSS ~ pages | RSS ~ bytes |", "|---|---|---|---|---|"]
        for t in tools:
            r = c["tools"][t]
            out.append(f"| {names[t]} | " + " | ".join(
                f"{r[k]['spearman']:.3f} / {r[k]['pearson_log_log']:.3f}"
                for k in ("time_vs_pages", "time_vs_bytes", "rss_vs_pages", "rss_vs_bytes")) + " |")
        out.append("")
    out += ["## Run-to-run comparison (anchor documents)", "",
            "Frozen-corpus documents measured again in the extension run: per document the ratio of the second run's "
            "value to the first run's; median and range over the anchors.", "",
            "| tool | anchors | time ratio: median (min–max) | peak RSS ratio: median (min–max) |", "|---|---|---|---|"]
    for t in tools:
        a = data["anchors"].get(t)
        if a:
            out.append(f"| {names[t]} | {a['n']} | {a['time_ratio_median']:.3f} ({a['time_ratio_min']:.3f}–"
                       f"{a['time_ratio_max']:.3f}) | {a['rss_ratio_median']:.3f} ({a['rss_ratio_min']:.3f}–"
                       f"{a['rss_ratio_max']:.3f}) |")
    out += ["", "## Documents left out", "",
            "Not processed by every tool in every repetition (the charts compare tools on the same documents only):", "",
            "| document | run | pages | not processed by |", "|---|---|---|---|"]
    for e in data["excluded"]:
        why = "; ".join(f"{names.get(t, t)} ({', '.join(f'{k} {v}x' for k, v in r.items()) if isinstance(r, dict) else r})"
                        for t, r in e["not_processed_by"].items())
        out.append(f"| {e['id']} | {e['run']} | {e['pages']} | {why or 'no page'} |")
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "charts")
    ap.add_argument("--corpus-run", type=Path, default=RUNS[0][1], help="results of the frozen-corpus run")
    ap.add_argument("--extension-run", type=Path, default=RUNS[1][1], help="results of the extension run")
    a = ap.parse_args()
    RUNS[:] = [("corpus", a.corpus_run.resolve()), ("extension", a.extension_run.resolve())]
    docs, excluded, anchors, reps = collect()
    data = analyse(docs, excluded, anchors, reps)
    a.out.mkdir(parents=True, exist_ok=True)
    n = data["documents"]
    by = data["documents_by_run"]
    reps = data["method"]["repetitions"]
    foot = [f"{n} documents ({by.get('corpus', 0)} from the frozen test corpus, {by.get('extension', 0)} added public "
            f"documents), each processed by every tool in all {reps} runs.",
            "One process per document on 8 logical CPUs (Intel Core i9-14900KF). Tools check different rule sets: "
            "DoX-Validate its full catalogue,",
            "the others PDF/UA-1. Method and data: docs/BENCHMARKS.md; regenerated with bench/charts.py."]
    sub_t = "Median wall-clock time per document in each page-count interval; band: interquartile range. Lower is better."
    sub_m = "Median peak resident memory (RSS) per document in each page-count interval; band: interquartile range. Lower is better."
    for theme in ("light", "dark"):
        (a.out / f"validation-time-{theme}.svg").write_text(line_chart(
            data, "time_s", theme, "Validation Time by PDF Page Count", sub_t,
            "Time per document (log scale)", tick_time, foot))
        (a.out / f"peak-memory-{theme}.svg").write_text(line_chart(
            data, "rss_mb", theme, "Peak Memory Usage by PDF Page Count", sub_m,
            "Peak RSS per document, MB (log scale)", tick_mb, foot))
        (a.out / f"summary-{theme}.svg").write_text(summary_chart(data, theme))
    (a.out / "chart-data.json").write_text(json.dumps(data, indent=1, ensure_ascii=False, sort_keys=True) + "\n")
    (a.out / "chart-tables.md").write_text(tables_md(data))
    print(f"{n} documents; charts and chart-data.json in {a.out.relative_to(ROOT) if a.out.is_relative_to(ROOT) else a.out}")


if __name__ == "__main__":
    main()
