#!/usr/bin/env python3
"""Compares dox-validate JSON reports with the construction-derived expectations of the corpus.

  expectations.py [--bin build/dox-validate] [--expected tests/corpus/fixtures/expected.json] [--dir DIR] [ID ...]

Prints one line per fixture (ok / the violated expectations); exit status 1 when an expectation is violated.
Used by tests/test_fixtures.py; standard library only.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def check(spec: dict, report: dict, exit_code: int, basis: str = "auto") -> list[str]:
    """The expectations of `spec` that `report` (and the exit status) violate; empty = all met.

    basis: the result basis the report was made with: "auto" (dox-validate's default) or "any" (--target any)."""
    problems = []
    reported = {f["rule"] for f in report.get("findings", [])}
    want = spec if basis == "auto" else spec["any_basis"]
    if exit_code not in want["exit_code"]:
        problems.append(f"exit status {exit_code}, expected one of {want['exit_code']} (basis {basis})")
    summary = report.get("summary", {})
    if summary.get("result") not in want["result"]:
        problems.append(f"summary.result {summary.get('result')!r}, expected one of {want['result']} (basis {basis})")
    if summary.get("basis") != basis:
        problems.append(f"summary.basis {summary.get('basis')!r}, expected {basis!r}")
    if basis == "auto" and spec.get("deciding_targets") and summary.get("targets") != spec["deciding_targets"]:
        problems.append(f"summary.targets {summary.get('targets')}, expected {spec['deciding_targets']}")
    if spec.get("processed") is not None and report.get("processed") != spec["processed"]:
        problems.append(f"processed {report.get('processed')}, expected {spec['processed']}")
    if spec.get("tagged") is not None and report.get("processed") and report["input"].get("tagged") != spec["tagged"]:
        problems.append(f"input.tagged {report['input'].get('tagged')}, expected {spec['tagged']}")
    if "pages" in spec and report.get("processed") and report["input"].get("pages") != spec["pages"]:
        problems.append(f"input.pages {report['input'].get('pages')}, expected {spec['pages']}")
    for r in spec["must_report"]:
        if r not in reported:
            problems.append(f"missing finding {r}")
    for group in spec.get("must_report_any", []):
        if not reported & set(group):
            problems.append(f"missing a finding of {group}")
    for r in spec["must_not_report"]:
        if r in reported:
            problems.append(f"unexpected finding {r}")
    allowed = set(spec["must_report"]) | set(spec["may_report"]) | {r for g in spec.get("must_report_any", []) for r in g}
    allowed |= {d["rule"] for d in spec.get("known_deviations", [])}   # documented, see deviations()
    for r in sorted(reported):
        if any(r.startswith(p) for p in spec["exclusive_prefixes"]) and r not in allowed and r not in spec["must_not_report"]:
            problems.append(f"unexpected finding {r} (not expected for this construction)")
    for target, status in spec["assessments"].items():
        got = report.get("assessments", {}).get(target, {}).get("status")
        if got != status:
            problems.append(f"assessment {target}: {got}, expected {status}")
    return problems


def deviations(spec: dict, report: dict) -> list[str]:
    """The documented deviations (rules reported against the normative reading, shared with the reference tool) present."""
    reported = {f["rule"] for f in report.get("findings", [])}
    return [d["rule"] for d in spec.get("known_deviations", []) if d["rule"] in reported]


def run(binary: str, pdf: Path, *extra: str) -> tuple[dict, int, str]:
    p = subprocess.run([binary, "--format", "json", *extra, str(pdf)], capture_output=True, timeout=600)
    return json.loads(p.stdout), p.returncode, p.stderr.decode("utf-8", "replace")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.environ.get("DOX_VALIDATE", str(ROOT / "build" / "dox-validate")))
    ap.add_argument("--expected", type=Path, default=ROOT / "tests" / "corpus" / "fixtures" / "expected.json")
    ap.add_argument("--dir", type=Path, help="directory of the PDFs (default: next to expected.json)")
    ap.add_argument("--target", default="auto", choices=("auto", "any"), help="result basis to run and check")
    ap.add_argument("--verbose", "-v", action="store_true", help="also list the rules reported for failing fixtures")
    ap.add_argument("ids", nargs="*")
    a = ap.parse_args()
    specs = json.loads(a.expected.read_text())["fixtures"]
    base = a.dir or a.expected.parent
    bad = 0
    for s in specs:
        if a.ids and s["id"] not in a.ids:
            continue
        report, code, _ = run(a.bin, base / s["file"], "--target", a.target)
        problems = check(s, report, code, a.target)
        print(f"{'ok  ' if not problems else 'FAIL'} {s['id']}" + ("" if not problems else ": " + "; ".join(problems)))
        if problems and a.verbose:
            print("      reported:", " ".join(sorted(f["rule"] for f in report.get("findings", []))))
        bad += bool(problems)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
