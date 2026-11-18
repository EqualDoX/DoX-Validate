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


# ----------------------------------------------------------------------------------------------------------------------
# a Private element where the parent has a fixed content model
# ----------------------------------------------------------------------------------------------------------------------
PRIVATE_TAG = "EQX-PAC-FailPrivateTagInvalidSyntax"
P_TAG = "EQX-PAC-FailPTagInvalidSyntax"
TR_KIDS = "EQX-UA1-7.2-10"      # a TR holds TH and TD only (ISO 32000-1 Table 337)


def _row_with(holder_role):
    """A table whose first body row gets a P as third kid: directly (holder_role None) or inside a Private / NonStruct."""
    d = TaggedDoc("private-in-tr")
    p = d.page()
    p.heading(1, "Table")
    t = p.table([["Region", "Requests"], ["North", "120"]])
    row = [k for k in t.kids if isinstance(k, SE)][1]
    holder = d.elem(holder_role, row) if holder_role else row
    p.para("Text of the misplaced element.", holder)
    return d


def test_private_in_a_row_is_reported_once_and_its_content_is_not(tmp_path):
    """TR(TD, TD, Private(P)): the Private is the element the row does not allow. What it holds is its private
    content (ISO 32000-1 14.8.4.2); PAC fails the Private and passes the P (PAC 26.1 read-out)."""
    rules = findings(tmp_path, _row_with("Private"), "private-in-tr")
    assert PRIVATE_TAG in rules and TR_KIDS in rules
    assert P_TAG not in rules


def test_p_directly_in_a_row_is_reported(tmp_path):
    rules = findings(tmp_path, _row_with(None), "p-in-tr")
    assert P_TAG in rules and TR_KIDS in rules


def test_p_in_a_nonstruct_in_a_row_is_reported(tmp_path):
    """NonStruct has no check of its own in PAC and is looked through: the P is judged against the row."""
    rules = findings(tmp_path, _row_with("NonStruct"), "nonstruct-in-tr")
    assert P_TAG in rules and TR_KIDS in rules and PRIVATE_TAG not in rules


# ----------------------------------------------------------------------------------------------------------------------
# header cells of a table organised with Headers and IDs (PAC THHasAssociatedCells)
# ----------------------------------------------------------------------------------------------------------------------
TH_ASSOCIATED = "EQX-PAC-THHasAssociatedCells_CheckTHs_TableHeaderCellHasNoAssociatedSubcells"


def _headers_table(first_id, row_header_headers, first_scope=None):
    """The table of veraPDF's 7.5-t01 files: a header row, then a row header and data cells; every data cell names its
    headers. first_id: /ID of the first header cell (None: none); row_header_headers: /Headers of the row header."""
    d = TaggedDoc("th-associated")
    p = d.page()
    p.heading(1, "Table")
    t = p.table([["Index", "Condition", "Section"], ["Row", "one", "two"]], scope=False)
    head, body = (cells(r) for r in t.kids if isinstance(r, SE))
    body[0].role = "TH"
    if first_id is not None:
        head[0].extra["ID"] = first_id
    head[1].extra["ID"], head[2].extra["ID"], body[0].extra["ID"] = "Condition", "Section", "Row"
    if row_header_headers is not None:
        body[0].extra["A"] = {"O": Name("Table"), "Headers": row_header_headers}
    body[1].extra["A"] = {"O": Name("Table"), "Headers": ["Row", "Condition"]}
    body[2].extra["A"] = {"O": Name("Table"), "Headers": ["Row", "Section"]}
    if first_scope:
        head[0].extra["A"] = {"O": Name("Table"), "Scope": Name(first_scope)}
    return d


def test_header_cell_named_by_headers_is_associated(tmp_path):
    assert TH_ASSOCIATED not in findings(tmp_path, _headers_table("Index", ["Index"]), "th-named")


def test_header_cell_with_an_empty_id_named_by_headers_is_associated(tmp_path):
    """veraPDF 7.5-t01-pass-d: /ID () on the header cell, /Headers [()] on the cell below it. An empty ID is an ID: the
    header cell is named, and both veraPDF (7.5-1) and PAC pass the file."""
    rules = findings(tmp_path, _headers_table("", [""]), "th-empty-id")
    assert TH_ASSOCIATED not in rules and "EQX-UA1-7.5-1" not in rules and "EQX-UA1-7.5-2" not in rules


def test_header_cell_with_an_id_nobody_names_is_reported(tmp_path):
    """veraPDF 7.5-t01-pass-c: the header cell has an ID, no cell names it and it has no Scope (PAC fails it)."""
    assert TH_ASSOCIATED in findings(tmp_path, _headers_table("Index", None), "th-unnamed")
    assert TH_ASSOCIATED in findings(tmp_path, _headers_table("", None), "th-unnamed-empty-id")


def test_header_cell_without_id_is_reported(tmp_path):
    """veraPDF 7.5-t01-pass-e: no ID on the header cell; the Headers entry that means it names nothing."""
    assert TH_ASSOCIATED in findings(tmp_path, _headers_table(None, ["Index"]), "th-no-id")


def test_header_cell_with_scope_is_associated(tmp_path):
    assert TH_ASSOCIATED not in findings(tmp_path, _headers_table(None, None, first_scope="Column"), "th-scope")
