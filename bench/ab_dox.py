#!/usr/bin/env python3
"""A/B comparison of two dox-validate binaries on selected corpus documents (one process per document, interleaved).

  python3 -I bench/ab_dox.py OLD_BINARY NEW_BINARY --label OLD:NEW [--reps 9] [--ids ID ...] [--out results/ab.csv]

Used to document the effect of a performance change (docs/BENCHMARKS.md). Both binaries run with
`--format json --jobs 1`, pinned to the same CPU set; the order alternates per repetition. Per run: wall, CPU and peak RSS
(bench/rusage_exec), the tool-reported timing_ms breakdown, and a sha256 over the findings (rule id and occurrence
count of every finding, minus the rules given by --ignore-rule, whose behaviour changed on purpose between the
compared commits). Equal digests show that both binaries did the same validation work.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench  # noqa: E402

DEFAULT_IDS = ["gen-medium-100-complex", "gen-large-500-complex", "gen-large-2000-complex", "gen-large-2000-simple",
               "irs-p17-2023", "nist-sp800-53r5", "nist-sp800-53Ar5", "govinfo-uscode-2022-title26", "ua1-good"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("old", type=Path)
    ap.add_argument("new", type=Path)
    ap.add_argument("--label", default="old:new")
    ap.add_argument("--reps", type=int, default=9)
    ap.add_argument("--ids", nargs="*", default=DEFAULT_IDS)
    ap.add_argument("--all", action="store_true", help="every corpus document (instead of --ids)")
    ap.add_argument("--cpus", default="0-7")
    ap.add_argument("--corpus", type=Path, default=bench.ROOT / "tests" / "corpus" / ".cache" / "full")
    ap.add_argument("--out", type=Path, default=bench.RESULTS / "ab_dox.csv")
    ap.add_argument("--ignore-rule", nargs="*", default=["EQX-WCAG-1.4.1-UseOfColorReview"],
                    help="rules excluded from the findings digest (intended changes between the binaries)")
    a = ap.parse_args()
    labels = a.label.split(":")
    docs, _ = bench.load_corpus(a.corpus, True)
    by_id = {d["id"]: d for d in docs}
    if a.all:
        a.ids = [d["id"] for d in sorted(docs, key=lambda d: d["id"])]
    missing = [i for i in a.ids if i not in by_id]
    if missing:
        sys.exit(f"not in the corpus: {missing}")
    os.sched_setaffinity(0, set(bench.parse_cpus(a.cpus)))
    scratch = Path("/dev/shm") / f"dox-ab-{os.getpid()}"
    scratch.mkdir(parents=True, exist_ok=True)
    bench.warm_page_cache([by_id[i]["path"] for i in a.ids])
    out, err, res = scratch / "out.json", scratch / "err.txt", scratch / "res.txt"
    a.out.parent.mkdir(parents=True, exist_ok=True)
    with a.out.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["rep", "binary", "item", "pages", "wall_s", "user_s", "sys_s", "maxrss_kb", "exit", "tool_total_ms",
                     "timing_ms", "findings_sha256", "processed"])
        for rep in range(a.reps):
            for doc_id in a.ids:
                d = by_id[doc_id]
                pair = [(labels[0], a.old), (labels[1], a.new)]
                if rep % 2:
                    pair.reverse()
                for label, binary in pair:
                    p = bench.spawn([str(binary), "--format", "json", "--jobs", "1", d["path"]], out, err, res)
                    _, status, ru = os.wait4(p.pid, 0)
                    m = bench.read_res(res, status, 0, ru)
                    try:
                        rep_json = json.loads(out.read_bytes())
                    except ValueError:
                        rep_json = {}
                    timing = rep_json.get("timing_ms", {})
                    findings = sorted((f["rule"], f["occurrences"]) for f in rep_json.get("findings", [])
                                      if f["rule"] not in a.ignore_rule)
                    digest = hashlib.sha256(json.dumps([rep_json.get("processed"), findings]).encode()).hexdigest()
                    w.writerow([rep, label, doc_id, d["pages"], f"{m['wall_s']:.6f}", f"{m['user_s']:.6f}",
                                f"{m['sys_s']:.6f}", m["maxrss_kb"], m["exit"], timing.get("total"),
                                json.dumps({k: round(v, 2) for k, v in timing.items()}, separators=(",", ":")), digest,
                                rep_json.get("processed")])
                    f.flush()
    for p in scratch.iterdir():
        p.unlink()
    scratch.rmdir()
    print(f"wrote {a.out}")


if __name__ == "__main__":
    main()
