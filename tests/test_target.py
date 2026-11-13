"""--target (result basis) and the legacy program name eqx-validate.

auto (default): the declared PDF/UA part (pdfuaid:part 2 -> pdfua-2, otherwise pdfua-1) and wcag-2.1-AA decide;
explicit targets: those decide; any: every error-severity finding and every required rule decide (the semantics of
eqx-validate, the default under that name and with --rules). The basis never changes findings or assessments.
"""
import os

import pytest

from conftest import BINARY, FIXTURES, Run, catalogue, run, strip_timing
from report_invariants import check_report

GOOD, UA2, FIGURE, GREY = (FIXTURES / f"{n}.pdf" for n in ("ua1-good", "ua2-good", "figure-no-alt", "contrast-lightgrey"))


def summary(*args):
    r = run("--format", "json", *args)
    rep = r.json()
    assert not check_report(rep, catalogue(), r.code)
    return rep["summary"], r.code, rep


def test_auto_follows_the_declared_part():
    s, code, rep = summary(UA2)
    assert rep["input"]["pdfua_part"] == 2
    assert (s["basis"], s["targets"], s["result"], code) == ("auto", ["pdfua-2", "wcag-2.1-AA"], "no_errors_found", 0)
    s, code, rep = summary(GOOD)
    assert rep["input"]["pdfua_part"] == 1
    assert (s["targets"], code) == (["pdfua-1", "wcag-2.1-AA"], 0)
    s, code, rep = summary(FIXTURES / "pdfuaid-missing.pdf")
    assert rep["input"]["pdfua_part"] is None
    assert (s["targets"], s["failed_targets"], code) == (["pdfua-1", "wcag-2.1-AA"], ["pdfua-1"], 1)
    s, code, rep = summary(FIXTURES / "metadata-missing.pdf")
    assert rep["input"]["pdfua_part"] is None and s["targets"][0] == "pdfua-1"


def test_explicit_targets():
    s, code, _ = summary("--target", "pdfua-2", GOOD)               # a PDF/UA-1 file judged as PDF/UA-2
    assert (s["basis"], s["targets"], s["failed_targets"], code) == ("targets", ["pdfua-2"], ["pdfua-2"], 1)
    s, code, _ = summary("--target", "pdfua-1", UA2)
    assert (s["failed_targets"], code) == (["pdfua-1"], 1)
    s, code, _ = summary("--target=wcag-2.1-AA", GREY)               # WCAG 1.4.3 is level AA
    assert (s["failed_targets"], code) == (["wcag-2.1-AA"], 1)
    s, code, _ = summary("--target", "wcag-2.1-A,wcag-2.2-A", GREY)  # ... so level A passes
    assert (s["targets"], s["result"], code) == (["wcag-2.1-A", "wcag-2.2-A"], "no_errors_found", 0)
    s, _, _ = summary("--target", "pdfua-1,pdfua-1,wcag-2.0-AAA", GOOD)
    assert s["targets"] == ["pdfua-1", "wcag-2.0-AAA"], "duplicates are dropped, order is kept"
    s, code, _ = summary("--target", "auto", UA2)
    assert (s["basis"], code) == ("auto", 0)


def test_not_processed_targets_incomplete():
    s, code, _ = summary(FIXTURES / "malformed-garbage.pdf")
    assert (s["result"], code) == ("not_processed", 2)
    assert s["incomplete_targets"] == s["targets"] == ["pdfua-1", "wcag-2.1-AA"]
    s, code, _ = summary("--target", "any", FIXTURES / "malformed-garbage.pdf")
    assert (s["result"], s["targets"], code) == ("not_processed", [], 2)


def test_incomplete_target_gives_exit_2():
    """The repaired file: every rule is incomplete, so the deciding targets are incomplete (exit 2), never a pass."""
    s, code, _ = summary(FIXTURES / "malformed-bad-xref.pdf")
    assert (s["result"], code) == ("incomplete", 2) and s["incomplete_targets"] == s["targets"]


@pytest.mark.parametrize("value", ["foo", "pdfua-3", "PDFUA-1", "", "pdfua-1,", ",pdfua-1", "any,pdfua-1",
                                   "auto,pdfua-1", "any,auto", "wcag-2.1"])
def test_invalid_target_is_a_usage_error(value):
    r = run("--format", "json", "--target", value, GOOD)
    assert r.code == 3 and r.stdout == b"" and r.stderr.strip()


def test_target_needs_a_value():
    assert run("--target").code == 3


def test_any_is_the_former_semantics():
    for f, code in ((GOOD, 1), (UA2, 1), (FIXTURES / "malformed-zero-byte.pdf", 2)):
        s, c, rep = summary("--target", "any", f)
        assert (s["basis"], s["targets"], c) == ("any", [], code)
        if rep["processed"]:
            assert s["result"] == "errors_found" and s["error_rules"] > 0


def test_basis_does_not_change_the_report_body():
    a = run("--format", "json", GOOD).json()
    for t in ("any", "pdfua-2", "wcag-2.2-AAA"):
        b = run("--format", "json", "--target", t, GOOD).json()
        for r in (a, b):
            r["summary"] = {k: v for k, v in r["summary"].items()
                            if k not in ("result", "exit_code", "basis", "targets", "failed_targets", "incomplete_targets")}
        assert strip_timing(a) == strip_timing(b), t


@pytest.fixture
def legacy(tmp_path):
    link = tmp_path / "eqx-validate"
    os.symlink(BINARY, link)

    def call(*args):
        import subprocess
        p = subprocess.run([str(link), *map(str, args)], capture_output=True, timeout=120)
        return Run(p.returncode, p.stdout, p.stderr.decode("utf-8", "replace"))
    return call


def test_legacy_name(legacy):
    v = legacy("--version")
    assert v.code == 0 and v.stdout.decode().startswith("eqx-validator ")
    r = legacy("--format", "json", GOOD)
    assert r.json()["summary"]["basis"] == "any" and r.code == 1, "eqx-validate keeps --target any as default"
    r = legacy("--format", "json", "--target", "auto", GOOD)
    assert r.json()["summary"]["basis"] == "auto" and r.code == 0, "an explicit --target wins"
    assert run("--version").stdout.decode().startswith("dox-validate ")


def test_build_tree_legacy_link():
    link = BINARY.parent / "eqx-validate"
    if not link.exists():
        pytest.skip("no eqx-validate link next to the binary")
    assert os.path.realpath(link) == os.path.realpath(BINARY)


def test_multi_file_exit_status_under_auto():
    r = run("--format", "json", GOOD, UA2)
    assert r.code == 0 and [x["summary"]["targets"][0] for x in r.json_lines()] == ["pdfua-1", "pdfua-2"]
    r = run("--format", "json", GOOD, FIGURE)
    assert r.code == 1
