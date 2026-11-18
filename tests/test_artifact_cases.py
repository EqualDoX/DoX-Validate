"""Artifact / tagged-content nesting cases built on the fly with the corpus generator (not part of the frozen corpus).

ISO 14289-1 7.1 keeps real content and artifacts apart; the Matterhorn Protocol 1.1 has one machine-checkable failure
condition for each way of nesting them:

    01-003  Content marked as Artifact is present inside tagged content.      -> EQX-UA1-7.1-1
    01-004  Tagged content is present inside content marked as Artifact.      -> EQX-UA1-7.1-2

Each construction below is one of the two (or both), and the test states how often each rule is reported. The original
observation: an Artifact nested in tagged content (four real-world documents, e.g. a whole page wrapped in
/Document <</MCID 0>> BDC with /Artifact sequences inside, or a form painted inside a Figure's sequence) was reported
under 7.1-2 as well as under 7.1-1, once per Artifact and once more per sequence inside it. veraPDF 1.30 does the same."""
from conftest import run
from pdfgen import MARGIN, MCR, Name, Stream, TaggedDoc

A_IN_T = "EQX-UA1-7.1-1"      # Artifact inside tagged content
T_IN_A = "EQX-UA1-7.1-2"      # tagged content inside an Artifact
FAIL = "fail"


def base():
    d = TaggedDoc("artifact-case")
    p = d.page()
    p.heading(1, "Artifacts")
    p.para("A paragraph of real content.")
    p.y -= 14
    return d, p


def text(p, s: str, dx: float = 0) -> bytes:
    return p.text_ops(s, MARGIN + dx, p.y, 11)


def own_mcid(p, se) -> int:
    """An MCID of page p owned by element se (ParentTree and /K), for a sequence written by hand."""
    m = MCR(p.index, p.mcid)
    p.mcid += 1
    p.mc_owners.append(se)
    se.kids.append(m)
    se.page = p.index
    return m.mcid


def counts(tmp_path, d, name):
    f = tmp_path / f"{name}.pdf"
    f.write_bytes(d.build())
    rep = run("--format", "json", f).json()
    found = {x["rule"]: x for x in rep["findings"]}
    return {r: found[r]["occurrences"] if r in found else 0 for r in (A_IN_T, T_IN_A)}, rep, found


def form(d, content: bytes, **entries):
    return d.w.add(Stream({"Type": Name("XObject"), "Subtype": Name("Form"), "BBox": [0, 0, 612, 792],
                           "Resources": {"Font": {"F1": d.fonts["F1"][0]}}, **entries}, content))


# ---------------------------------------------------------------------------------------------------------------------
# on the page
# ---------------------------------------------------------------------------------------------------------------------
def test_clean_document_reports_neither(tmp_path):
    d, p = base()
    p.artifact(text(p, "a decoration"))
    n, rep, _ = counts(tmp_path, d, "clean")
    assert n == {A_IN_T: 0, T_IN_A: 0}
    assert rep["assessments"]["pdfua-1"]["status"] == "automated_pass_manual_required"


def test_artifact_in_tagged_content_is_7_1_1_only(tmp_path):
    """/P <</MCID n>> BDC ... /Artifact BMC ... EMC ... EMC"""
    d, p = base()
    p._mc(d.elem("P"), text(p, "Tagged text") + b"/Artifact BMC\n" + text(p, "artifact", 100) + b"EMC\n")
    n, rep, found = counts(tmp_path, d, "a-in-t")
    assert n == {A_IN_T: 1, T_IN_A: 0}
    assert rep["assessments"]["pdfua-1"]["status"] == FAIL, "the document is still rejected for what it does contain"
    ua1 = next(s for s in found[A_IN_T]["references"]["standards"] if s["standard"] == "pdfua-1")
    assert ua1["matterhorn"] == ["01-003"]


def test_every_artifact_in_a_tagged_page_wrapper_is_7_1_1_only(tmp_path):
    """The real-world pattern: the page content in one tagged sequence, several Artifacts inside it."""
    d, p = base()
    body = text(p, "Body text")
    for i in range(5):
        body += b"/Artifact <</Type /Pagination>> BDC\n" + text(p, f"furniture {i}", 60 * (i + 1)) + b"EMC\n"
    p._mc(d.elem("P"), body)
    n, _, _ = counts(tmp_path, d, "a-in-t-many")
    assert n == {A_IN_T: 5, T_IN_A: 0}


def test_sequence_without_mcid_in_an_artifact_in_tagged_content_is_7_1_1_only(tmp_path):
    """/P <</MCID n>> BDC /Artifact BMC /PlacedGraphic BMC ... EMC EMC EMC: nothing becomes tagged inside the Artifact."""
    d, p = base()
    p._mc(d.elem("P"), text(p, "Tagged text") + b"/Artifact BMC\n/PlacedGraphic BMC\n" + text(p, "artifact", 100) +
          b"EMC\nEMC\n")
    n, _, _ = counts(tmp_path, d, "x-in-a-in-t")
    assert n == {A_IN_T: 1, T_IN_A: 0}


def test_tagged_content_in_an_artifact_is_7_1_2_only(tmp_path):
    """/Artifact BDC /P <</MCID n>> BDC ... EMC EMC"""
    d, p = base()
    mcid = own_mcid(p, d.elem("P"))
    p.artifact(b"/P <</MCID %d>> BDC\n" % mcid + text(p, "Tagged text inside an artifact") + b"EMC\n")
    n, rep, found = counts(tmp_path, d, "t-in-a")
    assert n == {A_IN_T: 0, T_IN_A: 1}
    assert rep["assessments"]["pdfua-1"]["status"] == FAIL
    ua1 = next(s for s in found[T_IN_A]["references"]["standards"] if s["standard"] == "pdfua-1")
    assert ua1["matterhorn"] == ["01-004"]
    assert f"MCID {mcid}" in found[T_IN_A]["locations"][0]["detail"]


def test_sequences_inside_tagged_content_in_an_artifact_are_7_1_2(tmp_path):
    """/Artifact BDC /P <</MCID n>> BDC /Span BMC ... EMC EMC EMC: the Span is part of that tagged content."""
    d, p = base()
    mcid = own_mcid(p, d.elem("P"))
    p.artifact(b"/P <</MCID %d>> BDC\n/Span BMC\n" % mcid + text(p, "Tagged text inside an artifact") + b"EMC\nEMC\n")
    n, _, _ = counts(tmp_path, d, "t-in-a-span")
    assert n == {A_IN_T: 0, T_IN_A: 2}


def test_tagged_in_artifact_in_tagged_is_both(tmp_path):
    """/P <</MCID a>> BDC /Artifact BMC /P <</MCID b>> BDC ... EMC EMC EMC: both nestings are there."""
    d, p = base()
    inner = own_mcid(p, d.elem("P"))
    p._mc(d.elem("P"), text(p, "Outer") + b"/Artifact BMC\n/P <</MCID %d>> BDC\n" % inner + text(p, "inner", 100) +
          b"EMC\nEMC\n")
    n, _, found = counts(tmp_path, d, "t-in-a-in-t")
    assert n == {A_IN_T: 1, T_IN_A: 1}
    assert f"MCID {inner}" in found[T_IN_A]["locations"][0]["detail"]


def test_artifact_that_is_tagged_content_itself_is_both(tmp_path):
    """/Artifact <</MCID n>> BDC ... EMC with the MCID owned by a structure element."""
    d, p = base()
    p._mc(d.elem("P"), text(p, "Artifact and tagged content at once"), tag="Artifact")
    n, _, _ = counts(tmp_path, d, "a-is-t")
    assert n == {A_IN_T: 1, T_IN_A: 1}


def test_artifact_with_an_mcid_no_element_owns_is_neither(tmp_path):
    """/Artifact <</MCID n>> BDC ... EMC whose MCID has no structure element: an artifact, not tagged content."""
    d, p = base()
    mcid = p.mcid
    p.mcid += 1
    p.mc_owners.append(None)
    p.ops.append(b"/Artifact <</MCID %d>> BDC\n" % mcid + text(p, "stale MCID") + b"EMC\n")
    n, _, _ = counts(tmp_path, d, "a-stale")
    assert n == {A_IN_T: 0, T_IN_A: 0}


# ---------------------------------------------------------------------------------------------------------------------
# through Form XObjects
# ---------------------------------------------------------------------------------------------------------------------
def test_form_painted_in_tagged_content_with_an_artifact_inside_is_7_1_1_only(tmp_path):
    """/Figure <</MCID n>> BDC /Fm0 Do EMC, the form holding /Artifact BDC /PlacedGraphic BMC ... EMC EMC (a placed
    graphic, as layout programs write it): the form's content is the Figure's, the Artifact lies inside it."""
    d, p = base()
    p.xobjects["Fm0"] = form(d, b"/Artifact <</Type /Layout>> BDC\n/PlacedGraphic BMC\nEMC\n0.5 g 80 600 40 20 re f\nEMC\n"
                                b"/Artifact <</Type /Layout>> BDC\n0.2 g 80 570 40 20 re f\nEMC\n")
    fig = d.elem("Figure")
    fig.extra["Alt"] = "A placed graphic"
    p._mc(fig, b"q /Fm0 Do Q\n")
    n, rep, found = counts(tmp_path, d, "form-a-in-t")
    assert n == {A_IN_T: 2, T_IN_A: 0}
    assert rep["assessments"]["pdfua-1"]["status"] == FAIL
    assert all("Artifact" in loc["detail"] for loc in found[A_IN_T]["locations"])


def test_form_painted_in_an_artifact_inside_tagged_content_is_7_1_1_only(tmp_path):
    """/P <</MCID n>> BDC /Artifact BMC /Fm0 Do EMC EMC, a sequence without MCID in the form: the one Artifact on the
    page is the defect; the form's content does not become tagged inside it."""
    d, p = base()
    p.xobjects["Fm0"] = form(d, b"/Span BMC\n" + text(p, "form text", 150) + b"EMC\n")
    p._mc(d.elem("P"), text(p, "Tagged text") + b"/Artifact BMC\nq /Fm0 Do Q\nEMC\n")
    n, _, _ = counts(tmp_path, d, "form-in-a-in-t")
    assert n == {A_IN_T: 1, T_IN_A: 0}


def _form_with_own_mcid(d, p, *, in_artifact: bool):
    """A form with /StructParents whose content is /P <</MCID 0>> BDC ... EMC, owned by a P element."""
    key = 1000
    ref = form(d, b"/P <</MCID 0>> BDC\n" + text(p, "Tagged text in a form") + b"EMC\n", StructParents=key)
    p.xobjects["Fm0"] = ref
    se = d.elem("P")
    if in_artifact:
        p.artifact(b"q /Fm0 Do Q\n")
    else:
        p.ops.append(b"q /Fm0 Do Q\n")

    def hook(doc, ctx):
        st = doc.w.get(ctx["catalog"]["StructTreeRoot"])
        doc.w.get(st["ParentTree"])["Nums"] += [key, [se.ref]]
        st["ParentTreeNextKey"] = key + 1
        e = doc.w.get(se.ref)
        e["Pg"] = ctx["page_refs"][0]
        e["K"] = [{"Type": Name("MCR"), "Pg": ctx["page_refs"][0], "Stm": ref, "MCID": 0}]
    d.hooks.append(hook)


def test_form_with_tagged_content_painted_in_an_artifact_is_7_1_2(tmp_path):
    d, p = base()
    _form_with_own_mcid(d, p, in_artifact=True)
    n, rep, _ = counts(tmp_path, d, "form-t-in-a")
    assert n == {A_IN_T: 0, T_IN_A: 1}
    assert rep["assessments"]["pdfua-1"]["status"] == FAIL


def test_form_with_tagged_content_painted_outside_artifacts_is_neither(tmp_path):
    d, p = base()
    _form_with_own_mcid(d, p, in_artifact=False)
    n, _, found = counts(tmp_path, d, "form-t")
    assert n == {A_IN_T: 0, T_IN_A: 0}
    assert "EQX-UA1-7.1-3" not in found, "the form's text is tagged"


def _form_as_content_item(d, p, *, in_artifact: bool):
    """A form that is a content item itself (/StructParent, an OBJR in the element), with a sequence inside it."""
    key = 1000
    ref = form(d, b"/Span BMC\n" + text(p, "Text of a form that is a content item") + b"EMC\n", StructParent=key)
    p.xobjects["Fm0"] = ref
    se = d.elem("P")
    if in_artifact:
        p.artifact(b"q /Fm0 Do Q\n")
    else:
        p.ops.append(b"q /Fm0 Do Q\n")

    def hook(doc, ctx):
        st = doc.w.get(ctx["catalog"]["StructTreeRoot"])
        doc.w.get(st["ParentTree"])["Nums"] += [key, se.ref]
        st["ParentTreeNextKey"] = key + 1
        e = doc.w.get(se.ref)
        e["Pg"] = ctx["page_refs"][0]
        e["K"] = [{"Type": Name("OBJR"), "Pg": ctx["page_refs"][0], "Obj": ref}]
    d.hooks.append(hook)


def test_form_content_item_painted_in_an_artifact_is_7_1_2(tmp_path):
    d, p = base()
    _form_as_content_item(d, p, in_artifact=True)
    n, _, _ = counts(tmp_path, d, "form-objr-in-a")
    assert n == {A_IN_T: 0, T_IN_A: 1}


def test_form_content_item_painted_outside_artifacts_is_neither(tmp_path):
    d, p = base()
    _form_as_content_item(d, p, in_artifact=False)
    n, _, _ = counts(tmp_path, d, "form-objr")
    assert n == {A_IN_T: 0, T_IN_A: 0}


# ---------------------------------------------------------------------------------------------------------------------
# --reference-dedup changes which uses of a reused stream are checked, not what the rule means
# ---------------------------------------------------------------------------------------------------------------------
def test_reference_dedup_gives_the_same_verdicts(tmp_path):
    d, p = base()
    p._mc(d.elem("P"), text(p, "Tagged text") + b"/Artifact BMC\n" + text(p, "artifact", 100) + b"EMC\n")
    f = tmp_path / "dedup.pdf"
    f.write_bytes(d.build())
    rules = {x["rule"] for x in run("--format", "json", "--reference-dedup", f).json()["findings"]}
    assert A_IN_T in rules and T_IN_A not in rules
