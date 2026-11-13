"""Consistency rules every dox-validate report must satisfy, whatever the input (used on every corpus file).

They follow from the documented semantics (dox-validate --help, src/report/output.cpp, src/report/assess.cpp):
exit status <-> summary.result <-> deciding targets (summary.basis) <-> finding severities <-> target assessments.
"""
from __future__ import annotations

COMBINE = {
    ("automated_pass_manual_required", "automated_pass_manual_required"): "both",
    ("fail", "fail"): "neither",
    ("automated_pass_manual_required", "fail"): "pdfua_only",
    ("fail", "automated_pass_manual_required"): "wcag_only",
    ("fail", "incomplete"): "pdfua_fail_wcag_incomplete",
    ("incomplete", "fail"): "wcag_fail_pdfua_incomplete",
}
RESULT_EXIT = {"not_processed": 2, "incomplete": 2, "errors_found": 1, "no_errors_found": 0}


def check_basis(r: dict) -> list[str]:
    """summary.result / exit_code against the result basis (--target)."""
    p: list[str] = []
    s, a = r["summary"], r["assessments"]
    if s["basis"] == "any":
        if s["targets"] or s["failed_targets"] or s["incomplete_targets"]:
            p.append("basis any with deciding targets")
        errors = any(f["severity"] == "error" for f in r["findings"])
        if r["processed"] and s["result"] == "incomplete":
            if not r["rules_incomplete"]:
                p.append("basis any: incomplete without an incomplete rule")
        elif r["processed"]:
            want = "errors_found" if errors else "no_errors_found"
            if s["result"] != want:
                p.append(f"basis any: result {s['result']}, expected {want}")
        return p
    if not s["targets"]:
        p.append(f"basis {s['basis']} without deciding targets")
    if s["basis"] == "auto":
        part = r["input"].get("pdfua_part")
        want_t = ["pdfua-2" if part == 2 else "pdfua-1", "wcag-2.1-AA"]
        if s["targets"] != want_t:
            p.append(f"basis auto: targets {s['targets']}, expected {want_t} for pdfua_part {part}")
    if not r["processed"]:
        if s["incomplete_targets"] != s["targets"] or s["failed_targets"]:
            p.append("not processed: every deciding target must be incomplete and none failed")
        return p
    failed = [t for t in s["targets"] if a[t]["status"] == "fail"]
    incomplete = [t for t in s["targets"] if a[t]["unevaluated_rules"]]
    if s["failed_targets"] != failed:
        p.append(f"failed_targets {s['failed_targets']}, assessments say {failed}")
    if s["incomplete_targets"] != incomplete:
        p.append(f"incomplete_targets {s['incomplete_targets']}, assessments say {incomplete}")
    want = "incomplete" if incomplete else "errors_found" if failed else "no_errors_found"
    if s["result"] != want:
        p.append(f"basis {s['basis']}: result {s['result']}, expected {want}")
    return p


def check_report(r: dict, catalogue: dict[str, tuple[str, str]], exit_code: int | None = None) -> list[str]:
    p: list[str] = []
    s = r["summary"]
    findings = r["findings"]
    by_rule = {f["rule"]: f for f in findings}
    if len(by_rule) != len(findings):
        p.append("a rule has more than one finding entry")
    # exit status and result
    if RESULT_EXIT[s["result"]] != s["exit_code"]:
        p.append(f"summary.result {s['result']} with exit_code {s['exit_code']}")
    if exit_code is not None and exit_code != s["exit_code"]:
        p.append(f"process exit status {exit_code} != summary.exit_code {s['exit_code']}")
    errors = [f for f in findings if f["severity"] == "error"]
    warnings = [f for f in findings if f["severity"] == "warning"]
    if s["error_rules"] != len(errors) or s["warning_rules"] != len(warnings):
        p.append("error_rules / warning_rules do not match the findings")
    if s["error_occurrences"] != sum(f["occurrences"] for f in errors):
        p.append("error_occurrences does not match the findings")
    if s["warning_occurrences"] != sum(f["occurrences"] for f in warnings):
        p.append("warning_occurrences does not match the findings")
    if s["result"] == "errors_found" and not errors:
        p.append("errors_found without an error-severity finding (warnings alone must not fail)")
    p += check_basis(r)
    if s["rules_evaluated"] != len(r["rules_evaluated"]):
        p.append("summary.rules_evaluated != len(rules_evaluated)")
    if s["rules_incomplete"] != len(r["rules_incomplete"]):
        p.append("summary.rules_incomplete != len(rules_incomplete)")
    if s["rules_not_applicable"] != len(r["rules_not_applicable"]):
        p.append("summary.rules_not_applicable != len(rules_not_applicable)")
    evaluated = set(r["rules_evaluated"])
    incomplete = {x["rule"] for x in r["rules_incomplete"]}
    not_applicable = {x["rule"] for x in r["rules_not_applicable"]}
    if evaluated & incomplete:
        p.append(f"rules both evaluated and incomplete: {sorted(evaluated & incomplete)[:5]}")
    if set(by_rule) & not_applicable:
        p.append("a not-applicable rule has a finding")
    if not evaluated | incomplete >= set(by_rule):
        p.append(f"findings for rules neither evaluated nor incomplete: {sorted(set(by_rule) - evaluated - incomplete)[:5]}")
    # findings against the catalogue
    pages = r["input"].get("pages")
    for f in findings:
        cat = catalogue.get(f["rule"])
        if cat is None:
            p.append(f"{f['rule']}: not in --list-rules")
            continue
        if cat[0] != f["severity"]:
            p.append(f"{f['rule']}: severity {f['severity']}, catalogue {cat[0]}")
        if cat[1] != f["module"]:
            p.append(f"{f['rule']}: module {f['module']}, catalogue {cat[1]}")
        if f["occurrences"] < 1 or f["non_pac_occurrences"] > f["occurrences"]:
            p.append(f"{f['rule']}: occurrence counts inconsistent")
        if len(f["locations"]) > f["occurrences"]:
            p.append(f"{f['rule']}: more locations than occurrences")
        for loc in f["locations"]:
            if loc["page"] is not None and not (pages and 1 <= loc["page"] <= pages):
                p.append(f"{f['rule']}: page {loc['page']} outside 1..{pages}")
    for x in r["rules_incomplete"] + r["rules_not_applicable"]:
        if x["rule"] not in catalogue:
            p.append(f"{x['rule']}: not in --list-rules")
    if any(x not in catalogue for x in r["rules_evaluated"]):
        p.append("rules_evaluated lists an unknown rule")
    # processed / not processed
    if not r["processed"]:
        if findings or r["rules_evaluated"]:
            p.append("not processed, but findings or evaluated rules")
        if s["result"] != "not_processed":
            p.append("not processed, but result " + s["result"])
    # assessments
    a = r["assessments"]
    for t in r["profiles"]:
        ta = a[t]
        failed, warned, uneval = set(ta["failed_rules"]), set(ta["warning_rules"]), set(ta["unevaluated_rules"])
        if (ta["status"] == "fail") != bool(failed):
            p.append(f"{t}: status {ta['status']} with {len(failed)} failed rule(s)")
        if ta["status"] == "automated_pass_manual_required" and uneval:
            p.append(f"{t}: automated pass with unevaluated rules")
        if ta["status"] == "incomplete" and not uneval and r["processed"]:
            p.append(f"{t}: incomplete without unevaluated rules")
        if not r["processed"] and ta["status"] != "incomplete":
            p.append(f"{t}: {ta['status']} on a file that was not processed")
        for rule in failed:
            if rule not in by_rule or by_rule[rule]["severity"] != "error":
                p.append(f"{t}: failed rule {rule} has no error-severity finding")
        for rule in warned:
            if rule not in by_rule:
                p.append(f"{t}: warning rule {rule} has no finding")
        if failed & warned:
            p.append(f"{t}: a rule is both failed and warning")
        if uneval - incomplete - (set(catalogue) - evaluated):
            p.append(f"{t}: an unevaluated rule was evaluated completely")
    deciding = r["profiles"] if s["basis"] == "any" else s["targets"]
    if s["exit_code"] == 0 and any(a[t]["status"] != "automated_pass_manual_required" for t in deciding):
        p.append("exit 0 with a deciding target that fails or is incomplete")
    for key, v in a.items():
        if "+" not in key:
            continue
        u, w = key.split("+")
        want = COMBINE.get((a[u]["status"], a[w]["status"]), "undetermined")
        if v["status"] != want:
            p.append(f"{key}: {v['status']}, expected {want} from {a[u]['status']} / {a[w]['status']}")
    return p
