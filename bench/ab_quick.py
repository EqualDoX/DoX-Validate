#!/usr/bin/env python3
"""Quick A/B of dox-validate binaries (or other tool variants) on the frozen corpus: one process per document,
interleaved, pinned to one CPU set; prints medians per size tier.

  python3 -I bench/ab_quick.py base=PATH new=PATH [more=PATH ...] [--tier small,medium,large] [--ids ID ...]
                               [--reps 5] [--cpus 2] [--docs] [--variants vellum-ua1,outloud-conf]

Each NAME=PATH is a dox-validate binary run as `PATH --format json --jobs 1 FILE`. --variants adds tool variants of
bench.py (for example vellum-ua1). Per document the median over the repetitions is taken; a tier shows the median over
its documents, the sum (for pages/s), and the median and maximum peak RSS. Only documents that every measured
variant processed are compared. For a development loop; the published numbers come from bench.py.
"""
from __future__ import annotations

import argparse
import json
import os
import statistics as st
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binaries", nargs="+", help="NAME=PATH")
    ap.add_argument("--tier", default="small,medium,large")
    ap.add_argument("--ids", nargs="*")
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--cpus", default="2")
    ap.add_argument("--docs", action="store_true", help="also print one line per document")
    ap.add_argument("--variants", default="", help="bench.py tool variants to add, comma-separated")
    ap.add_argument("--corpus", type=Path, default=bench.ROOT / "tests" / "corpus" / ".cache" / "full")
    ap.add_argument("--json-out", type=Path, help="write the per-document medians as JSON")
    a = ap.parse_args()
    bins = [b.split("=", 1) for b in a.binaries]
    variants = [v for v in a.variants.split(",") if v]
    names = [n for n, _ in bins] + variants
    docs, _ = bench.load_corpus(a.corpus, True)
    tiers = a.tier.split(",")
    docs = [d for d in docs if (d["id"] in a.ids if a.ids else d["tier"] in tiers)]
    os.sched_setaffinity(0, set(bench.parse_cpus(a.cpus)))
    scratch = Path("/dev/shm") / f"dox-abq-{os.getpid()}"
    scratch.mkdir(parents=True, exist_ok=True)
    bench.warm_page_cache([d["path"] for d in docs])
    out, err, res = scratch / "out.json", scratch / "err.txt", scratch / "res.txt"
    wall = {n: {d["id"]: [] for d in docs} for n in names}
    rss = {n: {d["id"]: [] for d in docs} for n in names}
    ok = {n: {d["id"]: True for d in docs} for n in names}
    for rep in range(a.reps):
        for d in docs:
            order = [(n, [p, "--format", "json", "--jobs", "1", d["path"]], "dox") for n, p in bins]
            order += [(v, bench.argv_for(v, [d["path"]]), v) for v in variants]
            k = rep % len(order)
            for name, argv, variant in order[k:] + order[:k]:
                p = bench.spawn(argv, out, err, res)
                _, status, ru = os.wait4(p.pid, 0)
                m = bench.read_res(res, status, 0, ru)
                try:
                    if variant == "dox":
                        processed = bool(json.loads(out.read_bytes()).get("processed"))
                    else:
                        _, failure = bench.check(variant, out, 1, m["exit"], [d["path"]])
                        processed = not failure
                except (ValueError, OSError):
                    processed = False
                ok[name][d["id"]] = ok[name][d["id"]] and processed
                wall[name][d["id"]].append(m["wall_s"])
                rss[name][d["id"]].append(float(m["maxrss_kb"] or 0) / 1024)
    for p in scratch.iterdir():
        p.unlink()
    scratch.rmdir()
    common = [d for d in docs if all(ok[n][d["id"]] for n in names)]
    med = {n: {d["id"]: st.median(wall[n][d["id"]]) for d in common} for n in names}
    mrss = {n: {d["id"]: st.median(rss[n][d["id"]]) for d in common} for n in names}

    def fmt(s):
        return f"{s * 1000:.2f} ms" if s < 1 else f"{s:.3f} s"

    print(f"{len(common)} of {len(docs)} documents processed by every variant; {a.reps} repetitions; CPUs {a.cpus}")
    groups = [("all", common)] + [(t, [d for d in common if d["tier"] == t]) for t in tiers]
    for label, ds in groups:
        if not ds:
            continue
        pages = sum(d["pages"] for d in ds)
        print(f"\n{label}: {len(ds)} documents, {pages} pages")
        base = names[0]
        for n in names:
            ws = [med[n][d["id"]] for d in ds]
            rs = [mrss[n][d["id"]] for d in ds]
            ratio = st.median(med[n][d["id"]] / med[base][d["id"]] for d in ds)
            print(f"  {n:14s} median {fmt(st.median(ws)):>10s}  sum {sum(ws):8.3f} s  {pages / sum(ws):8.0f} pages/s"
                  f"  RSS median {st.median(rs):6.1f} MB  max {max(rs):6.1f} MB  time/{base} {ratio:.3f}x")
    if a.docs:
        print()
        for d in sorted(common, key=lambda d: -med[names[0]][d["id"]]):
            print(f"  {d['id'][:40]:40s} {d['pages']:5d}p " + "  ".join(
                f"{n} {fmt(med[n][d['id']]):>10s} {mrss[n][d['id']]:6.1f} MB" for n in names))
    if a.json_out:
        a.json_out.write_text(json.dumps({n: {i: {"wall_s": med[n][i], "rss_mb": mrss[n][i]} for i in med[n]}
                                          for n in names}, indent=1))


if __name__ == "__main__":
    main()
