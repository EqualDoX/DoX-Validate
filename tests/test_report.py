"""Every report conforms to the JSON Schema and to the consistency rules (tests/report_invariants.py), under the default
result basis (auto) and under --target any."""
import hashlib

import pytest

from conftest import FIXTURES, catalogue, load_expected
from report_invariants import check_report

FILES = [s["file"] for s in load_expected()]
BASES = ["auto", "any"]


def pick(request, basis):
    return request.getfixturevalue("reports" if basis == "auto" else "reports_any")


@pytest.mark.parametrize("basis", BASES)
@pytest.mark.parametrize("name", FILES)
def test_schema(name, basis, request, schema_validator):
    report, _ = pick(request, basis)[name]
    errors = sorted(schema_validator.iter_errors(report), key=lambda e: list(e.absolute_path))
    assert not errors, "; ".join(f"{list(e.absolute_path)}: {e.message[:200]}" for e in errors[:5])


@pytest.mark.parametrize("basis", BASES)
@pytest.mark.parametrize("name", FILES)
def test_invariants(name, basis, request):
    report, code = pick(request, basis)[name]
    assert report["summary"]["basis"] == basis
    problems = check_report(report, catalogue(), code)
    assert not problems, "; ".join(problems[:10])


@pytest.mark.parametrize("name", FILES)
def test_basis_changes_only_the_summary(name, reports, reports_any):
    """--target decides the result only: findings, assessments and every other part are the same."""
    a, b = dict(reports[name][0]), dict(reports_any[name][0])
    for r in (a, b):
        r.pop("timing_ms")
        r["summary"] = {k: v for k, v in r["summary"].items()
                        if k not in ("result", "exit_code", "basis", "targets", "failed_targets", "incomplete_targets")}
    assert a == b


def test_schema_is_strict(reports, schema_validator):
    """The schema rejects unknown keys and wrong types (so it does not pass everything)."""
    report, _ = reports["ua1-good.pdf"]
    bad = dict(report, unexpected=1)
    assert list(schema_validator.iter_errors(bad))
    bad = dict(report, summary=dict(report["summary"], exit_code=7))
    assert list(schema_validator.iter_errors(bad))
    bad = dict(report, summary=dict(report["summary"], targets=["pdfua-3"]))
    assert list(schema_validator.iter_errors(bad))
    f = dict(report["findings"][0], severity="fatal")
    assert list(schema_validator.iter_errors(dict(report, findings=[f])))


@pytest.mark.parametrize("name", FILES)
def test_input_identity(name, reports):
    """input.path/sha256/bytes describe the file as given, also when it could not be processed."""
    data = (FIXTURES / name).read_bytes()
    report, _ = reports[name]
    assert report["input"]["path"] == str(FIXTURES / name)
    assert report["input"]["sha256"] == hashlib.sha256(data).hexdigest()
    assert report["input"]["bytes"] == len(data)
