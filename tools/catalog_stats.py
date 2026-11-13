"""Print coverage statistics of the rule catalogue as Markdown tables (the numbers in docs/STANDARDS.md).

    python3 -I tools/catalog_stats.py [CATALOG_DIR]

CATALOG_DIR defaults to the catalog/ directory next to this script's parent. Standard library only. The target
bitmasks are computed by tools/gen_catalog.py's own functions, so these numbers are the ones the build embeds.
"""
from __future__ import annotations

import collections
import importlib.util
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def load_gen_catalog():
    spec = importlib.util.spec_from_file_location("gen_catalog", HERE / "gen_catalog.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def table(header: list[str], rows: list[list]) -> str:
    numeric = [all(isinstance(r[i], int) for r in rows) for i in range(len(header))]
    out = ["| " + " | ".join(header) + " |", "|" + "|".join("---:" if n else "---" for n in numeric) + "|"]
    out += ["| " + " | ".join(str(c) for c in r) + " |" for r in rows]
    return "\n".join(out)


def main(argv: list[str]) -> int:
    gen = load_gen_catalog()
    cat = Path(argv[0]) if argv else HERE.parent / "catalog"
    rules = json.loads((cat / "rules.json").read_text("utf-8"))["rules"]
    wcag = {c["sc"]: c["level_by_version"] for c in json.loads((cat / "wcag2.json").read_text("utf-8"))["criteria"]}
    matrix = json.loads((cat / "matrix.json").read_text("utf-8"))

    print(f"## Rule catalogue ({len(rules)} rules)\n")
    fam = collections.OrderedDict((p, collections.Counter()) for p in
                                  ("EQX-UA1-", "EQX-UA2-", "EQX-TS32005-", "EQX-WTPDF-", "EQX-PAC-", "EQX-PACQ-", "EQX-WCAG-"))
    for r in rules:
        for p, c in fam.items():
            if r["id"].startswith(p):
                c["total"] += 1
                c[r["severity"]] += 1
                break
        else:
            raise SystemExit(f"unknown id prefix: {r['id']}")
    rows = []
    for p, c in fam.items():
        kinds = collections.Counter(r["kind"] for r in rules if r["id"].startswith(p))
        rows.append([f"`{p}*`", c["total"], c["error"], c["warning"], kinds["violation"], kinds["warning"], kinds["recommendation"]])
    tot = collections.Counter(r["severity"] for r in rules)
    kt = collections.Counter(r["kind"] for r in rules)
    rows.append(["**total**", len(rules), tot["error"], tot["warning"], kt["violation"], kt["warning"], kt["recommendation"]])
    print(table(["Prefix", "Rules", "severity error", "severity warning", "kind violation", "kind warning",
                 "kind recommendation"], rows))
    auto = collections.Counter(r.get("automation") for r in rules)
    print(f"\nAutomation class of the rules: {dict(auto)}; excluded: {sum(1 for r in rules if r.get('excluded'))}\n")

    print("## Rules per assessment target\n")
    rows = []
    for t in gen.SINGLE_TARGETS:
        bit = 1 << gen.TARGET_BIT[t]
        norm = [r for r in rules if gen.normative_mask(r, wcag) & bit]
        req = [r for r in norm if r.get("automation") in ("automatic", "partial") and not r.get("excluded")]
        can_fail = [r for r in norm if r["severity"] == "error"]
        concern_only = [r for r in rules if (gen.concerns_mask(r, wcag) & bit) and not (gen.normative_mask(r, wcag) & bit)]
        rows.append([f"`{t}`", len(norm), len(can_fail), len(req), len(concern_only)])
    print(table(["Target", "rules with a normative mapping", "of these error severity (can fail the target)",
                 "required rules", "non-normative (listed as warning_rules only)"], rows))
    no_target = [r["id"] for r in rules if r["severity"] == "error" and gen.normative_mask(r, wcag) == 0]
    print(f"\nError-severity rules without a normative mapping to any target ({len(no_target)}): "
          + ", ".join(f"`{i}`" for i in no_target) + "\n")

    print("## Requirements per target (catalog/matrix.json)\n")
    rows = []
    for t in gen.SINGLE_TARGETS:
        m = matrix["targets"][t]
        bc = m["by_class"]
        rows.append([f"`{t}`", len(m["requirements"]), bc.get("automatic", 0), bc.get("partial", 0), bc.get("manual", 0),
                     bc.get("unsupported", 0), len(m["manual_required"])])
    print(table(["Target", "requirements", "automatic", "partial", "manual", "unsupported", "manual_required"], rows))

    print("\n## WCAG success criteria with automated checks (class partial)\n")
    partial = collections.OrderedDict()
    for t in gen.SINGLE_TARGETS:
        if not t.startswith("wcag-"):
            continue
        for req in matrix["targets"][t]["requirements"]:
            if req["class"] == "partial":
                partial.setdefault(req["id"].removeprefix("WCAG-"), (req["title"], req.get("level"), len(req["rules"])))
    rows = []
    for sc, (title, level, n) in sorted(partial.items(), key=lambda kv: [int(x) for x in kv[0].split(".")]):
        versions = [v for v in gen.WCAG_VERSIONS if wcag.get(sc, {}).get(v)]
        rows.append([sc, title, level, ", ".join(versions), n])
    print(table(["SC", "Title", "Level", "WCAG versions", "mapped rules"], rows))

    en = matrix.get("en301549")
    if en:
        print("\n## EN 301 549 V3.2.1 clause 10 (derived from WCAG 2.1)\n")
        bc = collections.Counter(c["class"] for c in en["clauses"])
        print(f"{len(en['clauses'])} clauses: " + ", ".join(f"{k} {v}" for k, v in sorted(bc.items())))
        print("partial: " + ", ".join(f"{c['clause']} (SC {c['wcag21_sc']})" for c in en["clauses"] if c["class"] == "partial"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
