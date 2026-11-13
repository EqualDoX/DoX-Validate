"""Construction-derived expectations of the synthetic fixtures (tests/corpus/fixtures/expected.json), checked under the
default result basis (auto: the declared PDF/UA part and wcag-2.1-AA decide) and under --target any.

A fixture whose only violated expectations concern a rule listed for it in tests/corpus/known_defects.json is an expected
failure (strict: it fails again once the defect is fixed, so the list stays current). Documented deviations shared with
the reference tool (known_deviations in expected.json) are allowed and listed in tests/corpus/CORRECTNESS.md.
"""
import pytest

from conftest import GENERATED, known_defects, load_expected, load_generated, run
from expectations import check

SPECS = load_expected()


def judge(spec: dict, report: dict, code: int, basis: str) -> None:
    problems = check(spec, report, code, basis)
    defects = {d_id: d for d_id, d in known_defects().items() if spec["id"] in d.get("fixtures", [])}
    explained = [p for p in problems if any(r in p for d in defects.values() for r in d["rules"])]
    unexplained = [p for p in problems if p not in explained]
    assert not unexplained, "; ".join(unexplained)
    if explained:
        pytest.xfail("known validator defect " + ", ".join(defects) + ": " + "; ".join(explained))
    if defects:
        pytest.fail(f"listed as affected by {', '.join(defects)} but every expectation holds: update known_defects.json")


@pytest.mark.parametrize("spec", SPECS, ids=[s["id"] for s in SPECS])
def test_fixture_expectations(spec, reports):
    report, code = reports[spec["file"]]
    judge(spec, report, code, "auto")


@pytest.mark.parametrize("spec", SPECS, ids=[s["id"] for s in SPECS])
def test_fixture_expectations_target_any(spec, reports_any):
    report, code = reports_any[spec["file"]]
    judge(spec, report, code, "any")


def test_fixture_set_covers_the_requested_areas():
    ids = {s["id"] for s in SPECS}
    for prefix in ("ua1-good", "untagged", "lang-", "figure-", "parenttree-", "heading-", "table-", "link-", "field-",
                   "font-", "contrast-", "scanned-", "encrypted-", "malformed-", "edge-", "metadata-", "displaydoctitle-"):
        assert any(i.startswith(prefix) for i in ids), prefix


def test_clean_documents_exit_0():
    """A document with no automated failure for its declared PDF/UA part and WCAG 2.1 AA exits 0 by default."""
    zero = {s["id"] for s in SPECS if s["exit_code"] == [0]}
    assert {"ua1-good", "ua2-good", "ua1-good-5pages", "encrypted-owner-only"} <= zero


GEN = load_generated()


@pytest.mark.skipif(not GEN, reason="generated size tiers not built (python3 tests/corpus/generate.py --large)")
@pytest.mark.parametrize("basis", ["auto", "any"])
@pytest.mark.parametrize("spec", GEN, ids=[s["id"] for s in GEN])
def test_generated_tier_expectations(spec, basis):
    pdf = GENERATED / spec["file"]
    if not pdf.exists():
        pytest.skip("not generated")
    r = run("--format", "json", "--target", basis, pdf, timeout=600)
    judge(spec, r.json(), r.code, basis)
