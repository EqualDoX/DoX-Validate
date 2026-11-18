"""Command-line contract: options, usage errors, output streams, exit statuses."""
import json

import pytest

from conftest import FIXTURES, catalogue, run, strip_timing

GOOD = FIXTURES / "ua1-good.pdf"


def test_help_and_version():
    h = run("--help")
    assert h.code == 0 and b"usage: dox-validate" in h.stdout and h.stderr == ""
    assert run("-h").stdout == h.stdout
    v = run("--version")
    assert v.code == 0 and v.stdout.decode().startswith("dox-validate ") and v.stderr == ""


@pytest.mark.parametrize("args", [
    [],                                   # no file
    ["--no-such-option", str(GOOD)],
    ["--format", "xml", str(GOOD)],
    ["--format=yaml", str(GOOD)],
    ["--format"],                         # option without its value
    ["--jobs"],
    ["--rules", "EQX-NO-SUCH-PREFIX", str(GOOD)],
    ["--rules", "", str(GOOD)],
])
def test_usage_errors_exit_3(args):
    r = run(*args)
    assert r.code == 3, (args, r.code)
    assert r.stdout == b"", "usage errors write nothing to stdout"
    assert r.stderr.strip(), "usage errors are explained on stderr"


def test_list_rules():
    r = run("--list-rules")
    assert r.code == 0
    lines = r.stdout.decode().splitlines()
    rows = [line.split("\t") for line in lines]
    assert all(len(x) == 4 for x in rows)
    ids = [x[0] for x in rows]
    assert len(ids) == len(set(ids)) == 2009
    assert {x[1] for x in rows} == {"error", "warning"}
    assert all(x[2] for x in rows), "every rule has an owning module"
    assert catalogue()["EQX-WCAG-1.4.3-TextContrast"][0] == "error"


def test_self_check():
    r = run("--self-check")
    assert r.code == 0 and b"0 unowned" in r.stdout


def test_json_on_stdout_only():
    r = run("--format", "json", GOOD)
    report = r.json()                     # stdout is exactly one JSON document
    assert r.stdout.count(b"\n") == 1 and r.stdout.endswith(b"\n")
    assert report["schema"] == "eqx-validator-output/1"
    assert r.stderr == "", "a processed file with all rules evaluated writes no diagnostics"
    assert strip_timing(run("--json", GOOD).json()) == strip_timing(report)


def test_pretty_is_the_same_report():
    a = run("--format", "json", GOOD).json()
    b = run("--format", "json", "--pretty", GOOD)
    assert b.stdout.count(b"\n") > 50
    assert strip_timing(a) == strip_timing(json.loads(b.stdout))


def test_text_format():
    r = run(GOOD)
    out = r.stdout.decode()
    assert out.startswith(str(GOOD)) and "Result (pdfua-1, wcag-2.1-AA): NO ERRORS FOUND" in out
    assert r.code == 0
    bad = run(FIXTURES / "figure-no-alt.pdf")
    assert "Result (pdfua-1, wcag-2.1-AA): ERRORS FOUND - failing: pdfua-1, wcag-2.1-AA" in bad.stdout.decode()
    assert bad.code == 1
    legacy = run("--target", "any", GOOD).stdout.decode()
    assert "  Result: ERRORS FOUND - " in legacy


def test_missing_file_not_processed(tmp_path):
    r = run("--format", "json", tmp_path / "absent.pdf")
    assert r.code == 2
    rep = r.json()
    assert rep["processed"] is False and rep["summary"]["result"] == "not_processed"
    assert "not processed" in r.stderr, "the reason goes to stderr as well"


def test_json_lines_for_several_files():
    files = [FIXTURES / f for f in ("ua1-good.pdf", "malformed-zero-byte.pdf", "contrast-lightgrey.pdf")]
    r = run("--format", "json", "--jobs", "2", *files)
    lines = r.json_lines()
    assert [x["input"]["path"] for x in lines] == [str(f) for f in files], "one report per line, in argument order"
    assert r.code == 2, "the exit status is the highest over all files"
    assert "malformed-zero-byte.pdf: not processed" in r.stderr
    assert run("--format", "json", "--pretty", *files).stdout.count(b"\n") == 3, "--pretty is ignored for several files"


def test_exit_status_is_maximum(reports):
    files = ["ua1-good.pdf", "figure-no-alt.pdf", "contrast-black.pdf"]
    r = run("--format", "json", *[FIXTURES / f for f in files])
    assert r.code == max(reports[f][1] for f in files)
