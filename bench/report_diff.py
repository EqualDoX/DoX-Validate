#!/usr/bin/env python3
"""Whole-report comparison of a dox-validate binary against a recorded baseline (diagnostic level).

  python3 -I bench/report_diff.py record BASELINE_BINARY --out base.jsonl FILES_OR_DIRS... [--files-from LIST]
  python3 -I bench/report_diff.py check  NEW_BINARY --against base.jsonl [--baseline-bin BASELINE_BINARY]

`record` runs the binary on every PDF, one process per document and mode, and stores per document and mode the exit
status and a SHA-256 of stdout and stderr. `check` runs another binary on the same documents with the same modes and
lists every document whose exit status, report or diagnostics differ. With --baseline-bin, the baseline is run again
for each differing document and a unified diff of the two outputs is printed.

Nothing but the time measurements is removed before hashing: the `timing_ms` object of the JSON report and the
timing line of the text report. Everything else is compared byte for byte, so a finding that moves, loses a location,
changes its detail text or its order counts as a difference, and so does any change of the output format.

Modes (--modes, comma-separated; default json,text):
  json       --format json                     the eqx-validator-output/1 report (default result basis)
  text       -v                                the verbose text report (every target, every stored location)
  any        --format json --target any        the report under the other result basis
  dedup      --format json --reference-dedup   reference traversal (reused streams checked once)
  ua1rules   --format json --rules EQX-UA1-    rule selection
  wcagrules  --format json --rules EQX-WCAG-   rule selection (contrast engine only)
  maxloc     --format json --max-locations 3   location cap
  textmodel  --text-model                      eqx-text-model/1
  dump       --dump                            debug view of the parsed model
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import difflib
import hashlib
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

MODES = {
    "json": ["--format", "json", "--jobs", "1"],
    "text": ["-v", "--jobs", "1"],
    "any": ["--format", "json", "--jobs", "1", "--target", "any"],
    "dedup": ["--format", "json", "--jobs", "1", "--reference-dedup"],
    "ua1rules": ["--format", "json", "--jobs", "1", "--rules", "EQX-UA1-"],
    "wcagrules": ["--format", "json", "--jobs", "1", "--rules", "EQX-WCAG-"],
    "maxloc": ["--format", "json", "--jobs", "1", "--max-locations", "3"],
    "textmodel": ["--text-model"],
    "dump": ["--dump"],
}
TIMING_JSON = re.compile(rb'"timing_ms":\s*\{[^{}]*\}')
TIMING_TEXT = re.compile(rb"^.* ms\b.*$\n?", re.M)


def normalise(mode: str, out: bytes) -> bytes:
    if mode == "text":
        return TIMING_TEXT.sub(b"", out)
    if mode in ("textmodel", "dump"):
        return out
    return TIMING_JSON.sub(b'"timing_ms":{}', out)


def run_one(binary: str, mode: str, path: str, timeout: float):
    try:
        p = subprocess.run([binary, *MODES[mode], path], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=timeout)
        return p.returncode, normalise(mode, p.stdout), p.stderr
    except subprocess.TimeoutExpired:
        return "timeout", b"", b""


def digest(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def collect(args) -> list[str]:
    files: list[str] = []
    for item in args.inputs:
        p = Path(item)
        if p.is_dir():
            files += sorted(str(x) for x in p.rglob("*.pdf"))
        else:
            files.append(str(p))
    if args.files_from:
        files += [line.strip() for line in Path(args.files_from).read_text().splitlines() if line.strip()]
    seen, out = set(), []
    for f in files:
        if f not in seen:
            seen.add(f)
            out.append(f)
    return out


def pin(cpus: str | None):
    if not cpus:
        return
    s = set()
    for part in cpus.split(","):
        a, _, b = part.partition("-")
        s.update(range(int(a), int(b or a) + 1))
    os.sched_setaffinity(0, s)


def record(args):
    files = collect(args)
    modes = args.modes.split(",")
    pin(args.cpus)
    t0 = time.monotonic()
    with cf.ThreadPoolExecutor(args.workers) as ex, open(args.out, "w") as out:
        out.write(json.dumps({"binary": str(args.binary), "modes": modes, "files": len(files)}) + "\n")

        def job(path):
            rec = {"path": path}
            for m in modes:
                code, so, se = run_one(str(args.binary), m, path, args.timeout)
                rec[m] = [code, digest(so), digest(se), len(so)]
            return rec

        for i, rec in enumerate(ex.map(job, files), 1):
            out.write(json.dumps(rec) + "\n")
            if i % 500 == 0:
                print(f"  {i}/{len(files)} ({time.monotonic() - t0:.0f} s)", file=sys.stderr, flush=True)
    print(f"recorded {len(files)} documents x {len(modes)} modes in {time.monotonic() - t0:.1f} s -> {args.out}")


def show_diff(base_bin, new_bin, mode, path, timeout, limit):
    _, a, ae = run_one(str(base_bin), mode, path, timeout)
    _, b, be = run_one(str(new_bin), mode, path, timeout)

    def lines(x: bytes):
        s = x.decode("utf-8", "replace")
        if mode not in ("text", "dump"):
            try:  # one report element per line, key order kept
                s = json.dumps(json.loads(s), indent=1, ensure_ascii=False)
            except ValueError:
                pass
        return s.splitlines()

    n = 0
    for label, x, y in (("stdout", a, b), ("stderr", ae, be)):
        for line in difflib.unified_diff(lines(x), lines(y), f"baseline {label}", f"new {label}", lineterm="", n=2):
            print("      " + line[:400])
            n += 1
            if n >= limit:
                print("      ... (diff truncated)")
                return


def check(args):
    with open(args.against) as f:
        head = json.loads(f.readline())
        base = [json.loads(line) for line in f]
    modes = args.modes.split(",") if args.modes else head["modes"]
    missing = [m for m in modes if m not in head["modes"]]
    if missing:
        sys.exit(f"modes not in the baseline record: {missing}")
    if args.only:
        base = [r for r in base if any(s in r["path"] for s in args.only)]
    if args.sample and args.sample < len(base):
        step = len(base) / args.sample
        base = [base[int(i * step)] for i in range(args.sample)]
    pin(args.cpus)
    t0 = time.monotonic()
    diffs = []

    def job(rec):
        bad = []
        for m in modes:
            code, so, se = run_one(str(args.binary), m, rec["path"], args.timeout)
            got = [code, digest(so), digest(se), len(so)]
            if got != rec[m]:
                what = [n for n, x, y in zip(("exit", "stdout", "stderr", "length"), got, rec[m]) if x != y]
                bad.append((m, what, rec[m][0], code))
        return rec["path"], bad

    with cf.ThreadPoolExecutor(args.workers) as ex:
        for i, (path, bad) in enumerate(ex.map(job, base), 1):
            if bad:
                diffs.append((path, bad))
            if i % 500 == 0:
                print(f"  {i}/{len(base)} ({time.monotonic() - t0:.0f} s, {len(diffs)} differing)", file=sys.stderr,
                      flush=True)
    for path, bad in diffs:
        for m, what, c0, c1 in bad:
            print(f"DIFF {path} [{m}] {'+'.join(what)} (exit {c0} -> {c1})")
            if args.baseline_bin and args.show:
                show_diff(args.baseline_bin, args.binary, m, path, args.timeout, args.show)
    print(f"{len(base)} documents x {len(modes)} modes ({','.join(modes)}) in {time.monotonic() - t0:.1f} s: "
          f"{len(diffs)} documents differ from the baseline")
    sys.exit(1 if diffs else 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("record", "check"):
        p = sub.add_parser(name)
        p.add_argument("binary", type=Path)
        p.add_argument("--workers", type=int, default=4)
        p.add_argument("--cpus", help="CPU set for this process and the validator processes, e.g. 0-7")
        p.add_argument("--timeout", type=float, default=600)
        if name == "record":
            p.add_argument("inputs", nargs="*")
            p.add_argument("--files-from")
            p.add_argument("--out", required=True)
            p.add_argument("--modes", default="json,text")
        else:
            p.add_argument("--against", required=True)
            p.add_argument("--modes", help="subset of the recorded modes")
            p.add_argument("--baseline-bin", type=Path)
            p.add_argument("--show", type=int, default=40, help="diff lines per differing document (0 = none)")
            p.add_argument("--only", nargs="*", help="only documents whose path contains one of these strings")
            p.add_argument("--sample", type=int, help="evenly spaced sample of N documents")
    a = ap.parse_args()
    (record if a.cmd == "record" else check)(a)


if __name__ == "__main__":
    main()
