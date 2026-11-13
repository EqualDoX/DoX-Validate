"""Reports are deterministic: repeated runs, single-file and multi-file runs and any --jobs value give the same report
(timing_ms excepted)."""
from conftest import FIXTURES, load_expected, run, strip_timing

FILES = [FIXTURES / s["file"] for s in load_expected()]


def by_path(lines):
    return {x["input"]["path"]: strip_timing(x) for x in lines}


def test_repeated_runs_identical(reports):
    again = by_path(run("--format", "json", "--jobs", "1", *FILES).json_lines())
    for f in FILES:
        assert again[str(f)] == strip_timing(reports[f.name][0]), f.name


def test_jobs_do_not_change_reports(reports):
    for jobs in ("4", "16"):
        r = run("--format", "json", "--jobs", jobs, *FILES)
        multi = by_path(r.json_lines())
        assert len(multi) == len(FILES)
        for f in FILES:
            assert multi[str(f)] == strip_timing(reports[f.name][0]), (jobs, f.name)
        assert r.code == max(code for _, code in reports.values())


def test_text_report_deterministic():
    f = FIXTURES / "ua1-good-5pages.pdf"
    a, b = run(f).stdout.decode(), run(f).stdout.decode()
    strip = lambda s: "\n".join(line for line in s.splitlines() if " ms" not in line)  # noqa: E731 - the timing line
    assert strip(a) == strip(b)
