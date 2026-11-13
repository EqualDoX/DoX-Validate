"""Structure cases built on the fly with the corpus generator (not part of the frozen corpus): spans in tables, role-mapped
headings and nested lists. Expected verdicts follow from ISO 32000-1 14.8.4 / 14.8.5.7 and ISO 14289-1 7.4 / 7.5."""
from conftest import run
from pdfgen import SE, Name, TaggedDoc

IRREGULAR = {"EQX-UA1-7.2-41", "EQX-UA1-7.2-42", "EQX-UA1-7.2-43", "EQX-UA1-7.2-15"}


def findings(tmp_path, d, name):
    f = tmp_path / f"{name}.pdf"
    f.write_bytes(d.build())
    return {x["rule"] for x in run("--format", "json", f).json()["findings"]}


def cells(row: SE):
    return [k for k in row.kids if isinstance(k, SE)]


def test_colspan_makes_a_short_row_regular(tmp_path):
    d = TaggedDoc("colspan")
    p = d.page()
    p.heading(1, "Spans")
    t = p.table([["Region", "Requests", "Answered"], ["North", "120", "118"], ["Total", "238"]])
    rows = [k for k in t.kids if isinstance(k, SE)]
    cells(rows[2])[1].extra["A"] = {"O": Name("Table"), "ColSpan": 2}
    assert not findings(tmp_path, d, "colspan") & IRREGULAR


def test_rowspan_makes_a_short_row_regular(tmp_path):
    d = TaggedDoc("rowspan")
    p = d.page()
    p.heading(1, "Spans")
    t = p.table([["Region", "Requests", "Answered"], ["North", "120", "118"], ["95", "90"]])
    rows = [k for k in t.kids if isinstance(k, SE)]
    cells(rows[1])[0].extra["A"] = {"O": Name("Table"), "RowSpan": 2}
    assert not findings(tmp_path, d, "rowspan") & IRREGULAR


def test_wrong_colspan_is_irregular(tmp_path):
    d = TaggedDoc("colspan-wrong")
    p = d.page()
    p.heading(1, "Spans")
    t = p.table([["Region", "Requests", "Answered"], ["North", "120", "118"], ["Total", "238"]])
    rows = [k for k in t.kids if isinstance(k, SE)]
    cells(rows[2])[1].extra["A"] = {"O": Name("Table"), "ColSpan": 3}   # 1 + 3 = 4 columns instead of 3
    assert findings(tmp_path, d, "colspan-wrong") & IRREGULAR


def _role_mapped_headings(levels):
    d = TaggedDoc("rolemapped")
    p = d.page()
    for i, lv in enumerate(levels):
        p.heading(lv, f"Heading {i + 1}", role=f"Title{lv}")
        p.para("Text.")
    d.role_map = {f"Title{lv}": f"H{lv}" for lv in set(levels)}
    return d


def test_role_mapped_heading_skip_is_found(tmp_path):
    rules = findings(tmp_path, _role_mapped_headings([1, 3]), "rm-skip")
    assert "EQX-UA1-7.4.2-1" in rules and "EQX-PAC-FailNoHeadingsSkipped" in rules
    assert "EQX-UA1-7.1-5" not in rules, "the custom types are mapped"


def test_role_mapped_heading_sequence_passes(tmp_path):
    rules = findings(tmp_path, _role_mapped_headings([1, 2, 3]), "rm-ok")
    assert not rules & {"EQX-UA1-7.4.2-1", "EQX-PAC-FailNoHeadingsSkipped", "EQX-PAC-FailFirstHeadingIsH1", "EQX-UA1-7.1-5"}


def test_nested_list_in_lbody_is_allowed(tmp_path):
    d = TaggedDoc("nested-list")
    p = d.page()
    p.heading(1, "Lists")
    outer = p.list(["First", "Second"])
    lbody = next(k for k in next(k for k in outer.kids if isinstance(k, SE)).kids if isinstance(k, SE) and k.role == "LBody")
    p.list(["Inner one", "Inner two"], lbody)
    rules = findings(tmp_path, d, "nested-list")
    assert not {r for r in rules if r.startswith(("EQX-UA1-7.2-1", "EQX-UA1-7.2-2"))}, rules


def test_absurd_spans_make_the_table_undecidable(tmp_path):
    # A RowSpan that wraps the 32-bit row index back to an earlier row (the row count used to never end) and a ColSpan
    # near 2^63 that passed the span checks through wraparound (the cell used to be written past the grid): the table
    # rules are reported as not fully evaluated, never as passed, and the run ends promptly.
    for name, at, attrs in (("rowspan-wraps-back", (1, 0), {"RowSpan": 4294967295}),
                            ("colspan-near-int64-max", (1, 1), {"ColSpan": 9223372036854775807})):
        d = TaggedDoc(name)
        p = d.page()
        p.heading(1, "Spans")
        t = p.table([["A", "B"], ["1", "2"], ["3", "4"]])
        rows = [k for k in t.kids if isinstance(k, SE)]
        cells(rows[at[0]])[at[1]].extra["A"] = {"O": Name("Table"), **attrs}
        f = tmp_path / f"{name}.pdf"
        f.write_bytes(d.build())
        r = run("--format", "json", f, timeout=30)
        assert r.code == 2, (name, r.code)
        incomplete = {x["rule"] for x in r.json()["rules_incomplete"]}
        assert IRREGULAR <= incomplete, (name, IRREGULAR - incomplete)
