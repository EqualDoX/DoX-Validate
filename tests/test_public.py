"""Downloaded public documents (tests/corpus/fetch_public.py); every test skips when its file is not in the cache.

veraPDF corpus files carry an independent expectation in their name (<clause>-t<test>-<pass|fail>-<x>.pdf): a fail
file violates that one test, a pass file satisfies it. Where dox-validate merges a PAC condition into the same rule id,
only the occurrences of the veraPDF condition count (non_pac_occurrences). Disagreements that were analysed are listed in
tests/corpus/known_defects.json (validator defects) or tests/corpus/suite_disputes.json (the file does not show what its
name says, or a version difference); see CORRECTNESS.md.
"""
import json

import pytest

from conftest import CORPUS, PUBLIC, catalogue, known_defects, load_public, run
from report_invariants import check_report

DOCS = [d for d in load_public() if (PUBLIC / d["local"]).exists()]
DISPUTES_FILE = CORPUS / "suite_disputes.json"
DISPUTES = json.loads(DISPUTES_FILE.read_text())["disputes"] if DISPUTES_FILE.exists() else {}

pytestmark = pytest.mark.skipif(not DOCS, reason="no public documents downloaded (python3 tests/corpus/fetch_public.py)")

_cache: dict = {}


def report_of(doc):
    if doc["id"] not in _cache:
        r = run("--format", "json", PUBLIC / doc["local"], timeout=900)
        _cache[doc["id"]] = (r.json(), r.code)
    return _cache[doc["id"]]


@pytest.mark.parametrize("doc", DOCS, ids=[d["id"] for d in DOCS])
def test_public_report_valid(doc, schema_validator):
    report, code = report_of(doc)
    errors = list(schema_validator.iter_errors(report))
    assert not errors, errors[0].message[:300]
    problems = check_report(report, catalogue(), code)
    assert not problems, "; ".join(problems[:10])
    assert report["input"]["sha256"] == doc["sha256"]
    if report["processed"]:
        assert report["input"]["pages"] == doc["pages"], "page count differs from pdfinfo's"


SUITE = [d for d in DOCS if d["group"].startswith("verapdf-") and d["independent_expectation"]["rule"]]


@pytest.mark.parametrize("doc", SUITE, ids=[d["id"] for d in SUITE])
def test_verapdf_suite_verdict(doc):
    exp = doc["independent_expectation"]
    report, _ = report_of(doc)
    assert report["processed"], report.get("error")
    f = next((x for x in report["findings"] if x["rule"] == exp["rule"]), None)
    hit = f is not None and f["non_pac_occurrences"] > 0
    ok = hit if exp["verdict"] == "fail" else not hit
    if ok:
        if doc["id"] in DISPUTES:
            pytest.fail(f"listed in suite_disputes.json but agrees now: {doc['id']}")
        return
    why = f"{exp['rule']} {'not reported' if exp['verdict'] == 'fail' else 'reported'}; the file states: {exp['statement']}"
    for d_id, d in known_defects().items():
        if doc["id"] in d.get("public", []):
            pytest.xfail(f"known validator defect {d_id}: {why}")
    if doc["id"] in DISPUTES:
        pytest.xfail(f"analysed disagreement ({DISPUTES[doc['id']]['class']}): {DISPUTES[doc['id']]['reason']}")
    pytest.fail(why)
