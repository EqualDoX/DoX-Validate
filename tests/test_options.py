"""--max-pages, --rules, --max-locations, --max-memory-mb."""
from conftest import FIXTURES, GENERATED, run, strip_timing
import pytest

FIVE = FIXTURES / "ua1-good-5pages.pdf"


def test_max_pages_limit():
    r = run("--format", "json", "--max-pages", "4", FIVE)
    rep = r.json()
    assert r.code == 2 and rep["processed"] is False and "too many pages" in rep["error"]
    assert all(rep["assessments"][t]["status"] == "incomplete" for t in rep["profiles"])
    r = run("--format", "json", "--max-pages", "5", FIVE)
    assert r.json()["processed"] is True and r.json()["input"]["pages"] == 5


def test_rules_prefix_selects_wcag_only():
    r = run("--format", "json", "--rules", "EQX-WCAG-", FIXTURES / "contrast-lightgrey.pdf")
    rep = r.json()
    assert rep["rule_selection"] == "EQX-WCAG-"
    assert rep["summary"]["basis"] == "any", "--rules defaults to --target any"
    assert {f["rule"] for f in rep["findings"]} == {"EQX-WCAG-1.4.3-TextContrast"}
    assert rep["rules_evaluated"] and all(x.startswith("EQX-WCAG-") for x in rep["rules_evaluated"])
    # every other rule stays unevaluated, never passed: no target can be an automated pass
    assert rep["assessments"]["pdfua-1"]["status"] == "incomplete"
    assert rep["assessments"]["wcag-2.1-AA"]["status"] == "fail"
    assert rep["assessments"]["wcag-2.1-A"]["status"] == "incomplete"
    # summary and exit status are relative to the selection (src/report/output.cpp summarize: in_selection)
    assert r.code == 1 and rep["summary"]["result"] == "errors_found"


def test_rules_prefix_without_findings():
    r = run("--format", "json", "--rules", "EQX-WCAG-", FIXTURES / "contrast-black.pdf")
    rep = r.json()
    assert rep["findings"] == []
    assert r.code == 0 and rep["summary"]["result"] == "no_errors_found"
    # ...but no target is an automated pass: the rules outside the selection stay unevaluated
    assert all(rep["assessments"][t]["status"] == "incomplete" for t in rep["profiles"])


def test_max_locations_keeps_occurrences():
    full = run("--format", "json", FIXTURES / "lang-missing.pdf").json()
    cut = run("--format", "json", "--max-locations", "1", FIXTURES / "lang-missing.pdf").json()
    f_full = {f["rule"]: f for f in full["findings"]}
    for f in cut["findings"]:
        assert len(f["locations"]) <= 1
        assert f["occurrences"] == f_full[f["rule"]]["occurrences"]


@pytest.mark.skipif(not (GENERATED / "gen-large-2000-complex.pdf").exists(), reason="large tier not generated")
def test_memory_limit_never_passes_on_a_partial_reading():
    """Under an address-space limit a file is either validated completely (the same report as without a limit) or
    reported as not processed / incomplete (exit 2): what does not fit is never a pass and never a crash.
    (The limit is N MiB per file plus the reserved validation stacks, so small N may still fit.)"""
    pdf = GENERATED / "gen-large-2000-complex.pdf"
    full = run("--format", "json", "--max-memory-mb", "0", pdf, timeout=600)
    for mb in ("1", "48"):
        r = run("--format", "json", "--max-memory-mb", mb, pdf, timeout=600)
        assert r.code in (0, 1, 2), r.stderr[-300:]
        rep = r.json()
        if r.code == 2:
            assert rep["summary"]["result"] in ("not_processed", "incomplete")
        else:
            assert strip_timing(rep) == strip_timing(full.json()) and r.code == full.code
