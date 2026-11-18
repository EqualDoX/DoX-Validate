#!/usr/bin/env python3
"""Deterministic generator of the synthetic test corpus for dox-validate.

  generate.py                  write the committed fixtures to tests/corpus/fixtures/ (+ expected.json)
  generate.py --check          regenerate them into a temporary directory and compare sha256 with the committed files
  generate.py --large [--out DIR] [--only ID ...]
                               write the on-demand size tiers (medium, large) to tests/corpus/.cache/generated/
                               (+ generated.json); several MB, never committed

Every expectation below comes from the construction of the file and a normative requirement (ISO 14289-1 = PDF/UA-1,
ISO 14289-2 = PDF/UA-2, ISO 32000-1/-2, the Matterhorn Protocol 1.1, WCAG 2.x), never from the output of a validator.
Standard library only (Python 3.10+).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pdfgen import (MARGIN, MCR, PAGE_H, PAGE_W, Annot, Name, Raw, SE, Str, Stream, TaggedDoc, Writer,  # noqa: E402
                    scanned_image, simple_truetype)

HERE = Path(__file__).resolve().parent
FIXTURES = HERE / "fixtures"
CACHE = HERE / ".cache" / "generated"

AUTO_PASS, FAIL, INCOMPLETE = "automated_pass_manual_required", "fail", "incomplete"
# A PDF/UA-1-clean construction whose change does not touch an automatable WCAG 2.1 AA condition either.
UA1_PASS = {"pdfua-1": AUTO_PASS, "wcag-2.1-AA": AUTO_PASS}
WCAG_PASS = {t: AUTO_PASS for t in ("wcag-2.0-A", "wcag-2.1-AA", "wcag-2.2-AA")}
ALL_TARGETS = ["pdfua-1", "pdfua-2", "wtpdf-1.0-accessibility", "wtpdf-1.0-reuse"] + [
    f"wcag-{v}-{l}" for v in ("2.0", "2.1", "2.2") for l in ("A", "AA", "AAA")]
NOT_PROCESSED = {t: INCOMPLETE for t in ALL_TARGETS}

# Consequences of a PDF/UA-1 document that the PDF/UA-2 and WTPDF profiles always reject (ISO 14289-2 5 / 8.2.5.2 / 8.8,
# WTPDF 1.0 6.1): pdfuaid:part is 1 instead of 2, no pdfuaid:rev, the Document element is not in the PDF 2.0 namespace,
# outline destinations are not structure destinations, no WTPDF declarations. The fixtures list them as expected
# findings of the UA-2 profile; under `--target any` they make the exit status 1 for any PDF/UA-1 document.
UA1_DOC_UA2_FINDINGS = ["EQX-UA2-5-2", "EQX-UA2-5-5", "EQX-UA2-8.2.5.2-2", "EQX-WTPDF-6.1.2-1", "EQX-WTPDF-6.1.3-1"]

FIXTURE_FNS: list = []


def fixture(fn):
    FIXTURE_FNS.append(fn)
    return fn


def spec(fid: str, purpose: str, construction: str, basis: list[str], *, must_report=(), must_not_report=(), must_report_any=(),
         may_report=(), exclusive=("EQX-UA1-",), exit_code=1, result="errors_found", assessments=None,
         tagged: bool | None = True, processed: bool = True, ua2_consequences: bool = True, category="small",
         deviations=(), declared_part: int | None = 1, auto=None) -> dict:
    """exit_code/result describe `--target any` (every error-severity finding decides). The default basis (auto: the
    declared PDF/UA part and wcag-2.1-AA) is derived from the expected statuses of those two targets, or given as
    auto=(exit_codes, results) where they are not determined by the construction."""
    must = list(must_report)
    if ua2_consequences and processed:
        must += [r for r in UA1_DOC_UA2_FINDINGS if r not in must]
    exit_code = exit_code if isinstance(exit_code, list) else [exit_code]
    result = result if isinstance(result, list) else [result]
    deciding = ["pdfua-2" if declared_part == 2 else "pdfua-1", "wcag-2.1-AA"]
    a = assessments or {}
    if auto is not None:
        auto_exit, auto_result = auto
    elif processed is not True or result == ["incomplete"]:
        auto_exit, auto_result = [2], result
    elif any(a.get(t) == FAIL for t in deciding):
        auto_exit, auto_result = [1], ["errors_found"]
    elif all(a.get(t) == AUTO_PASS for t in deciding):
        auto_exit, auto_result = [0], ["no_errors_found"]
    else:
        raise ValueError(f"{fid}: the construction does not determine the auto result; pass auto=")
    return {"id": fid, "file": f"{fid}.pdf", "purpose": purpose, "construction": construction, "normative_basis": basis,
            "expectation_source": "construction", "must_report": must,
            "must_report_any": [list(g) for g in must_report_any], "must_not_report": list(must_not_report),
            "may_report": list(may_report), "exclusive_prefixes": list(exclusive) if processed else [],
            "deciding_targets": deciding if processed is True else None,
            "exit_code": auto_exit, "result": auto_result,
            "any_basis": {"exit_code": exit_code, "result": result}, "processed": processed,
            "assessments": assessments or {}, "tagged": tagged, "size_tier": category,
            "known_deviations": [{"rule": r, "reason": why} for r, why in deviations]}


# --------------------------------------------------------------------------------------------------------------------------
# document content
# --------------------------------------------------------------------------------------------------------------------------
WORDS = ("the report describes data quality access service public method review section result table figure value "
         "measure annual budget plan city water energy transport school health office record level change total "
         "average sample period region policy").split()


def sentence(seed: int, n: int = 14) -> str:
    x, out = seed * 2654435761 % 2**32, []
    for _ in range(n):
        x = (x * 1103515245 + 12345) % 2**31
        out.append(WORDS[(x >> 8) % len(WORDS)])
    return " ".join(out).capitalize() + "."


def article(d: TaggedDoc, *, figure_alt: str | None = "Bar chart: budget per year, rising from 10 to 40",
            link: bool = True, field: bool = True, table: bool = True, lst: bool = True) -> None:
    """The standard one-page body: H1, P, Figure, H2, P, Table, L, Link, form field."""
    p = d.page()
    p.heading(1, "Annual service report")
    p.para(sentence(1, 30))
    p.figure(figure_alt)
    p.heading(2, "Results by region")
    p.para(sentence(2, 20))
    if table:
        p.table([["Region", "Requests", "Answered"], ["North", "120", "118"], ["South", "95", "90"]])
    if lst:
        p.list(["Open data portal", "Annual budget plan"])
    if link:
        p.link("example.org/report", "https://example.org/report", prefix="Full report:")
    if field:
        p.text_field("email", "Email address for updates")


def good_doc(seed: str, **kw) -> TaggedDoc:
    d = TaggedDoc(seed, **kw)
    article(d)
    return d


GOOD_CONSTRUCTION = ("One-page tagged PDF 1.7 article (H1, P, Figure with Alt and BBox, H2, P, Table with TH/Scope, L/LI/Lbl/LBody, "
                     "Link with OBJR and /Contents, text field with /TU in a Form element); every content item is either "
                     "marked content owned by a structure element (MCID + ParentTree) or an Artifact; Catalog /Lang en-US, "
                     "/MarkInfo /Marked true, /ViewerPreferences /DisplayDocTitle true, XMP dc:title + pdfuaid:part 1, "
                     "outline entries for the headings, embedded TrueType fonts (Corpus Sans) with WinAnsiEncoding and "
                     "ToUnicode, /Tabs /S on the page with annotations.")


def variant(fid: str, purpose: str, construction: str, basis: list[str], must: list[str], *, doc_kw=None, mutate=None,
            assessments=None, may=(), exclusive=("EQX-UA1-",), must_not=(), body=None, any_of=(), deviations=(), **skw):
    """A fixture derived from the good document by one deliberate change."""
    def fn():
        d = TaggedDoc(fid, **(doc_kw or {}))
        (body or article)(d)
        if mutate:
            mutate(d)
        a = {"pdfua-1": FAIL} if any(r.startswith("EQX-UA1-") for r in must + [x for g in any_of for x in g]) else {}
        a.update(assessments or {})
        return d.build(), spec(fid, purpose, GOOD_CONSTRUCTION + " Change: " + construction, basis, must_report=must,
                               may_report=may, exclusive=exclusive, must_not_report=must_not, assessments=a,
                               must_report_any=any_of, deviations=deviations,
                               declared_part=(doc_kw or {}).get("ua_part", 1), **skw)
    fn.__name__ = fid
    FIXTURE_FNS.append(fn)
    return fn


# --------------------------------------------------------------------------------------------------------------------------
# known-good documents
# --------------------------------------------------------------------------------------------------------------------------
@fixture
def ua1_good():
    fid = "ua1-good"
    return good_doc(fid).build(), spec(
        fid, "Positive control: a PDF/UA-1 document whose machine-checkable requirements are all met.", GOOD_CONSTRUCTION,
        ["ISO 14289-1:2014 clauses 5-7 (all machine-checkable Matterhorn conditions are satisfied by construction)",
         "WCAG 2.x 1.1.1, 1.3.1, 1.4.3, 2.4.2, 3.1.1, 4.1.2 (automatable parts)"],
        must_not_report=["EQX-WCAG-1.4.3-TextContrast", "EQX-PAC-FailFigureHasBBox", "EQX-PACQ-FailOutlineIsNotPresentDespiteHeadings"],
        exclusive=("EQX-UA1-", "EQX-PAC-Fail", "EQX-PACQ-Fail", "EQX-WCAG-"), assessments={**UA1_PASS, **WCAG_PASS})


@fixture
def ua2_good():
    fid = "ua2-good"
    d = good_doc(fid, version="2.0", ua_part=2, ua_rev=2024, outline=False, pdf2_namespace=True,
                 wtpdf="http://pdfa.org/declarations/wtpdf#accessibility1.0 http://pdfa.org/declarations/wtpdf#reuse1.0")
    return d.build(), spec(
        fid, "Positive control for PDF/UA-2 and WTPDF 1.0: the machine-checkable requirements are met.",
        GOOD_CONSTRUCTION.replace("PDF 1.7", "PDF 2.0") + " PDF 2.0 header, XMP pdfuaid:part 2 + pdfuaid:rev 2024, WTPDF "
        "accessibility and reuse declarations (pdfd:declarations), structure elements in the PDF 2.0 namespace, widget "
        "/Contents, no outline (PDF/UA-2 8.8 would require structure destinations).",
        ["ISO 14289-2:2024 clauses 5 and 8", "WTPDF 1.0 clause 6.1",
         "ISO 14289-1 clause 5 / 6.1: a PDF/UA-1 file has pdfuaid:part 1 and a %PDF-1.n header, so the PDF/UA-1 profile fails"],
        must_report=["EQX-UA1-5-2", "EQX-UA1-6.1-1"], exclusive=("EQX-UA2-", "EQX-WTPDF-", "EQX-UA1-"),
        assessments={"pdfua-1": FAIL, "pdfua-2": AUTO_PASS, "wtpdf-1.0-accessibility": AUTO_PASS, "wtpdf-1.0-reuse": AUTO_PASS,
                     **WCAG_PASS},
        ua2_consequences=False, declared_part=2)


@fixture
def ua1_good_multipage():
    fid = "ua1-good-5pages"
    d = TaggedDoc(fid)
    for i in range(5):
        p = d.page()
        p.heading(1 if i == 0 else 2, f"Chapter {i + 1}")
        for k in range(4):
            p.para(sentence(10 * i + k, 40))
        p.figure(f"Diagram {i + 1}: three stacked boxes")
    # a paragraph continued across pages 4 and 5: one P owning MCIDs on two pages (MCR dictionaries with /Pg)
    se = d.elem("P")
    d.pages[3].text_block(se, sentence(99, 20))
    d.pages[4].text_block(se, sentence(100, 20))
    return d.build(), spec(
        fid, "Positive control, 5 pages, an element whose content spans two pages (MCR dictionaries with /Pg).",
        "Five tagged pages, each H1/H2 + four P + Figure with Alt; one P owns marked content on pages 4 and 5. Otherwise as ua1-good.",
        ["ISO 32000-1 14.7.4.2 (marked-content references on another page than the element's /Pg)", "ISO 14289-1 clause 7"],
        exclusive=("EQX-UA1-", "EQX-PAC-Fail", "EQX-PACQ-Fail", "EQX-WCAG-"), assessments={**UA1_PASS, **WCAG_PASS})


# --------------------------------------------------------------------------------------------------------------------------
# tagging and structure
# --------------------------------------------------------------------------------------------------------------------------
@fixture
def untagged():
    fid = "untagged-text"
    d = TaggedDoc(fid, tagged=False, mark_info={}, outline=False)
    p = d.page(furniture=False)
    p.untagged_text("Annual service report")
    for k in range(6):
        p.untagged_text(sentence(k, 12))
    return d.build(), spec(
        fid, "An untagged text document (no structure tree, no MarkInfo).",
        "One page of text drawn with an embedded font, no StructTreeRoot, no MarkInfo, no marked content; Lang, XMP, "
        "DisplayDocTitle present.",
        ["ISO 14289-1 7.1 (Matterhorn 01-005 content neither tagged nor artifact; 01-006 no structure tree)",
         "ISO 14289-1 6.2 / ISO 32000-1 14.7.1 (MarkInfo /Marked true)"],
        must_report=["EQX-UA1-7.1-11", "EQX-UA1-6.2-1", "EQX-UA1-7.1-3"], may_report=[],
        assessments={"pdfua-1": FAIL}, tagged=False, ua2_consequences=False)


variant("marked-false", "Structure tree present but MarkInfo /Marked false.", "/MarkInfo << /Marked false >>.",
        ["ISO 14289-1 6.2 (Marked shall be true); Matterhorn 06-002"], ["EQX-UA1-6.2-1"],
        doc_kw={"mark_info": {"Marked": False}})

variant("suspects-true", "MarkInfo /Suspects true.", "/MarkInfo << /Marked true /Suspects true >>.",
        ["ISO 14289-1 7.1 (Suspects shall be false if present); Matterhorn 01-002"], ["EQX-UA1-7.1-4"],
        doc_kw={"mark_info": {"Marked": True, "Suspects": True}})


def _untagged_line(d: TaggedDoc):
    d.pages[0].untagged_text("This sentence is real content outside any marked-content sequence.")


variant("content-untagged", "Real content that is neither tagged nor an artifact.",
        "One text line drawn outside any marked-content sequence.",
        ["ISO 14289-1 7.1 paragraph 3; Matterhorn 01-005"], ["EQX-UA1-7.1-3"], mutate=_untagged_line,
        may=["EQX-PAC-FailMarkedContentIsInLegalPosition"], exclusive=("EQX-UA1-",))


def _artifact_inside_tagged(d: TaggedDoc):
    p = d.pages[0]
    se = d.elem("P")
    p.y -= 14
    p._mc(se, p.text_ops("Tagged text", MARGIN, p.y, 11) + b"/Artifact BMC\n" + p.text_ops("artifact", MARGIN + 100, p.y, 11) + b"EMC\n")


variant("artifact-in-tagged", "An Artifact sequence nested inside tagged content.",
        "A P element's marked-content sequence contains a nested /Artifact BMC ... EMC.",
        ["ISO 14289-1 7.1 paragraph 2 (content marked as Artifact should not be inside tagged content); Matterhorn 01-003",
         "The nested Artifact contains no tagged content, so Matterhorn 01-004 (tagged content inside content marked as "
         "Artifact) is not violated; veraPDF 1.30.2 reports its 7.1-2 for this file too"],
        ["EQX-UA1-7.1-1"], mutate=_artifact_inside_tagged, must_not=["EQX-UA1-7.1-2"])


def _tagged_inside_artifact(d: TaggedDoc):
    p = d.pages[0]
    se = d.elem("P")
    p.y -= 14
    m = MCR(p.index, p.mcid)
    p.mcid += 1
    p.mc_owners.append(se)
    se.kids.append(m)
    se.page = p.index
    p.artifact(b"/P <</MCID %d>> BDC\n" % m.mcid + p.text_ops("Tagged text inside an artifact", MARGIN, p.y, 11) + b"EMC\n")


variant("tagged-in-artifact", "Tagged content nested inside an Artifact sequence.",
        "A P element owns an MCID whose sequence is nested inside /Artifact BDC ... EMC.",
        ["ISO 14289-1 7.1 paragraph 2 (tagged content should not be inside content marked as Artifact); Matterhorn 01-004",
         "No Artifact lies inside tagged content, so Matterhorn 01-003 is not violated"],
        ["EQX-UA1-7.1-2"], mutate=_tagged_inside_artifact, may=["EQX-PAC-FailMarkedContentIsInLegalPosition"],
        must_not=["EQX-UA1-7.1-1"])


def _no_parent_tree(d: TaggedDoc):
    d.parent_tree = False


variant("parenttree-missing", "Structure tree without ParentTree.", "StructTreeRoot has no /ParentTree (and no ParentTreeNextKey).",
        ["ISO 32000-1 14.7.4.4 (ParentTree required when elements own content items); PAC StructuralParentTreeMissing"],
        ["EQX-PAC-FailStructuralParentTreeMissing"], mutate=_no_parent_tree, exclusive=(),
        assessments={"pdfua-1": FAIL})


def _drop_pt_page(d: TaggedDoc):
    d.drop_parent_tree_pages.add(0)


variant("parenttree-entry-missing", "ParentTree without the entry for the page's StructParents key.",
        "The ParentTree /Nums array omits key 0 (the page's /StructParents) while keeping the annotation entries.",
        ["ISO 32000-1 14.7.4.4 (each StructParents key needs an entry); PAC StructuralParentTreeEntryMissing"],
        ["EQX-PAC-FailStructuralParentTreeEntryMissing"], mutate=_drop_pt_page, exclusive=(),
        assessments={"pdfua-1": FAIL})


def _drop_structparents(d: TaggedDoc):
    d.drop_struct_parents.add(0)


variant("page-structparents-missing", "Page with marked content but without /StructParents.",
        "The page dictionary has no /StructParents although its content has MCIDs.",
        ["ISO 32000-1 14.7.4.4 (StructParents required for pages with marked-content sequences owned by elements); PAC "
         "StructuralParentTreePageHasNoStructPatents"],
        ["EQX-PAC-FailStructuralParentTreePageHasNoStructPatents"], mutate=_drop_structparents, exclusive=(),
        assessments={"pdfua-1": FAIL})


def _swap_owner(d: TaggedDoc):
    p = d.pages[0]
    p.mc_owners[0], p.mc_owners[1] = p.mc_owners[1], p.mc_owners[0]


variant("parenttree-wrong-owner", "ParentTree entries that point to the wrong elements.",
        "The ParentTree array of the page lists the owners of MCID 0 and MCID 1 swapped (the elements' /K are correct).",
        ["ISO 32000-1 14.7.4.4 (the ParentTree maps each MCID to the element whose K contains it); PAC StructuralParentTreeInvalidReference"],
        ["EQX-PAC-FailStructuralParentTreeInvalidReference"], mutate=_swap_owner, exclusive=(),
        assessments={"pdfua-1": FAIL})


def _omit_p(d: TaggedDoc):
    next(se for se in d.elements if se.role == "P").omit_parent = True


variant("structelem-no-parent", "A structure element without its /P entry.", "The first P element has no /P (parent) key.",
        ["ISO 32000-1 14.7.2 Table 323 (P required); ISO 14289-1 7.1"], ["EQX-UA1-7.1-12"], mutate=_omit_p,
        may=["EQX-PAC-FailStructureElementHasParentKey2"],
        deviations=[("EQX-UA1-7.1-3", "the P element is reachable from the root through /K and owns its MCIDs through the "
                     "ParentTree, so its content is tagged; veraPDF 1.30.2 also reports 7.1-3 (it does not attach an element "
                     "whose /P differs from its actual parent), see CORRECTNESS.md")])


def _nonstandard(d: TaggedDoc):
    next(se for se in d.elements if se.role == "P").role = "BodyText"


variant("rolemap-missing", "A non-standard structure type without a role mapping.",
        "The first P element is renamed /BodyText; the StructTreeRoot has no RoleMap.",
        ["ISO 14289-1 7.1 paragraph 6 (non-standard types shall be mapped); Matterhorn 02-001"], ["EQX-UA1-7.1-5"],
        mutate=_nonstandard, may=["EQX-PAC-WarnMarkedContentIsInLegalPosition"])


def _mapped(d: TaggedDoc):
    _nonstandard(d)
    d.role_map = {"BodyText": "P"}


variant("rolemap-good", "Positive control: a non-standard type mapped to P.",
        "The first P element is renamed /BodyText and RoleMap << /BodyText /P >>.",
        ["ISO 14289-1 7.1 paragraph 6; ISO 32000-1 14.8.6"], [], mutate=_mapped,
        must_not=["EQX-UA1-7.1-5", "EQX-UA1-7.1-6", "EQX-UA1-7.1-7"], assessments={**UA1_PASS})


def _circular(d: TaggedDoc):
    _nonstandard(d)
    d.role_map = {"BodyText": "Para", "Para": "BodyText"}


variant("rolemap-circular", "A circular role mapping.", "First P renamed /BodyText; RoleMap << /BodyText /Para /Para /BodyText >>.",
        ["ISO 14289-1 7.1 paragraph 6 (no circular mapping); Matterhorn 02-002"], ["EQX-UA1-7.1-6"], mutate=_circular,
        may=["EQX-UA1-7.1-5", "EQX-PAC-WarnMarkedContentIsInLegalPosition"])


def _remap_std(d: TaggedDoc):
    d.role_map = {"P": "Span"}


variant("rolemap-standard-remapped", "A standard structure type remapped.", "RoleMap << /P /Span >>.",
        ["ISO 14289-1 7.1 paragraph 7 (standard tags shall not be remapped); Matterhorn 02-003"], ["EQX-UA1-7.1-7"],
        mutate=_remap_std)


def _li_outside_l(d: TaggedDoc):
    lse = next(se for se in d.elements if se.role == "L")
    li = next(k for k in lse.kids if isinstance(k, SE))
    lse.kids.remove(li)
    d.top.add(li)
    lbody = next(k for k in li.kids if isinstance(k, SE) and k.role == "LBody")
    li.kids.remove(lbody)
    d.top.add(lbody)


variant("list-wrong-nesting", "List items outside a list.",
        "The first LI is moved from L to Document, and its LBody from LI to Document.",
        ["ISO 32000-1 14.8.4.3.3 (LI is a child of L, LBody a child of LI); veraPDF PDF/UA-1 7.2 containment rules (Matterhorn 09-004)"],
        ["EQX-UA1-7.2-17", "EQX-UA1-7.2-18"], mutate=_li_outside_l,
        may=["EQX-PAC-FailLITagInvalidSyntax", "EQX-PAC-FailLBodyTagInvalidSyntax", "EQX-PAC-WarnLITagAsRoot",
             "EQX-PACQ-FailLIContainsExactlyOneBody", "EQX-UA2-8.2.5.25-2"])


# --------------------------------------------------------------------------------------------------------------------------
# headings
# --------------------------------------------------------------------------------------------------------------------------
def _headings_body(levels):
    def body(d: TaggedDoc):
        p = d.page()
        for i, lv in enumerate(levels):
            if isinstance(lv, str):
                p.heading(None, f"Heading {i + 1}", role=lv)
            else:
                p.heading(lv, f"Heading {i + 1}")
            p.para(sentence(i, 16))
    return body


variant("heading-skipped-level", "A numbered heading skips a level (H1 then H3).", "Headings H1, H3 (no H2) each followed by a P.",
        ["ISO 14289-1 7.4.2; Matterhorn 14-003 (numbered heading levels in descending sequence are skipped)"],
        ["EQX-PAC-FailNoHeadingsSkipped", "EQX-UA1-7.4.2-1"], body=_headings_body([1, 3]))

variant("heading-first-not-h1", "The first numbered heading is H2.", "Headings H2, H3, each followed by a P.",
        ["ISO 14289-1 7.4.2; Matterhorn 14-002 (numbered headings used, but the first is not H1)"],
        ["EQX-PAC-FailFirstHeadingIsH1", "EQX-UA1-7.4.2-1"], body=_headings_body([2, 3]))

variant("heading-good-sequence", "Positive control: H1, H2, H3, H2, H3 in order.", "Headings H1 H2 H3 H2 H3 each followed by a P.",
        ["ISO 14289-1 7.4.2"], [], body=_headings_body([1, 2, 3, 2, 3]),
        must_not=["EQX-PAC-FailNoHeadingsSkipped", "EQX-PAC-FailFirstHeadingIsH1", "EQX-UA1-7.4.2-1"],
        exclusive=("EQX-UA1-", "EQX-PAC-Fail", "EQX-PACQ-Fail"), assessments={**UA1_PASS})

variant("heading-h-and-hn", "Both H and H1-H6 used.", "Headings H1 and H (unnumbered) as siblings in Document.",
        ["ISO 14289-1 7.4.4 (documents shall be either strongly or weakly structured, not both); Matterhorn 14-007"],
        ["EQX-UA1-7.4.4-2"], body=_headings_body([1, "H"]), may=["EQX-UA1-7.4.4-3", "EQX-PAC-FailCaptionHeadingStructureTypesNotMixed"])

variant("heading-two-h-in-node", "Two unnumbered H elements in one node.", "Two H elements as children of Document.",
        ["ISO 14289-1 7.4.4 (each node shall contain at most one child H tag); Matterhorn 14-006"],
        ["EQX-UA1-7.4.4-1"], body=_headings_body(["H", "H"]))


def _no_outline(d: TaggedDoc):
    d.outline = False


variant("headings-without-outline", "Headings present, no document outline.", "No /Outlines in the Catalog.",
        ["PAC 2026 quality check 'Headings are present without bookmarks' (not a PDF/UA-1 requirement; related to WCAG 2.4.5 "
         "Multiple Ways, which other means can satisfy)"], ["EQX-PACQ-FailOutlineIsNotPresentDespiteHeadings"], mutate=_no_outline,
        exclusive=("EQX-UA1-",), assessments={**UA1_PASS})

# --------------------------------------------------------------------------------------------------------------------------
# figures and alternative text
# --------------------------------------------------------------------------------------------------------------------------
variant("figure-no-alt", "A Figure without alternative text.", "The Figure element has neither /Alt nor /ActualText.",
        ["ISO 14289-1 7.3 (Figure tags shall include an alternative representation or replacement text); Matterhorn 13-004",
         "WCAG 2.x 1.1.1 Non-text Content (level A)"],
        ["EQX-UA1-7.3-1"], body=lambda d: article(d, figure_alt=None),
        assessments={"wcag-2.0-A": FAIL, "wcag-2.1-AA": FAIL, "wcag-2.2-AA": FAIL})


def _figure_actualtext(d: TaggedDoc):
    p = d.page()
    p.heading(1, "Figure with replacement text")
    p.figure(None, actual_text="Budget 2024")
    p.para(sentence(3))


variant("figure-actualtext", "Positive control: a Figure with /ActualText instead of /Alt.",
        "One Figure with /ActualText and no /Alt, otherwise as ua1-good (H1, P).",
        ["ISO 14289-1 7.3 (alternative representation OR replacement text)"], [], body=_figure_actualtext,
        must_not=["EQX-UA1-7.3-1"], assessments={**UA1_PASS})


def _figure_no_bbox(d: TaggedDoc):
    p = d.page()
    p.heading(1, "Figure without bounding box")
    p.figure("A grey box", bbox=False)
    p.para(sentence(4))


variant("figure-no-bbox", "A Figure on one page without a BBox attribute.", "Figure with /Alt and no Layout /BBox.",
        ["ISO 32000-1 14.8.5.4.3 Table 344 (BBox required for an illustration that appears in its entirety on one page)"],
        ["EQX-PAC-FailFigureHasBBox"], body=_figure_no_bbox, exclusive=("EQX-UA1-",), assessments={"pdfua-1": FAIL})


def _note(ids):
    def body(d: TaggedDoc):
        p = d.page()
        p.heading(1, "Notes")
        par = p.para(sentence(5))
        for k, nid in enumerate(ids):
            n = p.para(f"{k + 1} A note.", par, role="Note")
            if nid is not None:
                n.extra["ID"] = Str(nid.encode())
    return body


variant("note-no-id", "A Note element without ID.", "Note (child of P) without /ID.",
        ["ISO 14289-1 7.9 (Note tags shall have an ID entry); Matterhorn 19-003"], ["EQX-UA1-7.9-1"], body=_note([None]),
        may=["EQX-PACQ-WarnNoteContainsLbl", "EQX-PACQ-WarnNoteTagInsidePTag", "EQX-PACQ-WarningNoteHasReferringLinkOnPage"])

variant("note-duplicate-id", "Two Note elements with the same ID.", "Two Note elements with /ID (n1).",
        ["ISO 14289-1 7.9 (unique ID); Matterhorn 19-004"], ["EQX-UA1-7.9-2"], body=_note(["n1", "n1"]),
        may=["EQX-PACQ-WarnNoteContainsLbl", "EQX-PACQ-WarnNoteTagInsidePTag", "EQX-PACQ-WarningNoteHasReferringLinkOnPage"])

# --------------------------------------------------------------------------------------------------------------------------
# tables
# --------------------------------------------------------------------------------------------------------------------------
def _table_body(rows, **kw):
    def body(d: TaggedDoc):
        p = d.page()
        p.heading(1, "Table test")
        p.table(rows, **kw)
    return body


ROWS = [["Region", "Requests", "Answered"], ["North", "120", "118"], ["South", "95", "90"]]

variant("table-good", "Positive control: a regular table with column headers and Scope.", "3x3 table, TH with /Scope /Column.",
        ["ISO 14289-1 7.5; ISO 32000-1 14.8.4.3.4"], [], body=_table_body(ROWS),
        must_not=["EQX-UA1-7.5-1", "EQX-UA1-7.2-42", "EQX-PAC-WarnTablesAreRegular"], assessments={**UA1_PASS})

variant("table-irregular", "A row with fewer cells than the others.", "3 columns in rows 1 and 2, 2 cells in row 3 (no ColSpan).",
        ["ISO 14289-1 7.5 / ISO 32000-1 14.8.4.3.4; veraPDF PDF/UA-1 7.2-42 (rows shall have the same number of columns)"],
        [], any_of=[["EQX-UA1-7.2-42", "EQX-UA1-7.2-43"]], body=_table_body([ROWS[0], ROWS[1], ["South", "95"]]),
        may=["EQX-PAC-WarnTablesAreRegular", "EQX-UA1-7.2-41", "EQX-UA2-8.2.5.26-3", "EQX-UA2-8.2.5.26-4",
             "EQX-UA2-8.2.5.26-1"])

variant("table-th-no-scope", "TH cells without Scope in a table without Headers/IDs.", "3x3 table, TH without /Scope.",
        ["ISO 14289-1 7.5; Matterhorn 15-003 (TH without Scope in a table not organised with Headers and IDs)"],
        ["EQX-UA1-7.5-1"], body=_table_body(ROWS, scope=False), may=["EQX-UA1-7.5-2"])

variant("table-td-in-table", "TD cells directly inside Table (no TR).", "Table rows are flattened: TH/TD are children of Table.",
        ["ISO 32000-1 14.8.4.3.4 (TH and TD are children of TR); veraPDF PDF/UA-1 7.2-3 / 7.2-8 / 7.2-9"],
        ["EQX-UA1-7.2-3", "EQX-UA1-7.2-9"], body=_table_body(ROWS, row_parent_role=None),
        may=["EQX-UA1-7.2-8", "EQX-UA1-7.2-41", "EQX-UA1-7.2-42", "EQX-UA1-7.2-43", "EQX-UA1-7.5-1", "EQX-UA1-7.5-2",
             "EQX-UA1-7.2-15", "EQX-PAC-FailTDTagInvalidSyntax", "EQX-PAC-FailTHTagInvalidSyntax", "EQX-PAC-FailTableTagInvalidSyntax",
             "EQX-PACQ-WarningCompleteTables", "EQX-PAC-WarnTablesAreRegular"])

# --------------------------------------------------------------------------------------------------------------------------
# document language
# --------------------------------------------------------------------------------------------------------------------------
# What a Lang value has to be. ISO 14289-1 7.2 refers to ISO 32000-1 14.9.2; its 14.9.2.2 reads: "A language identifier
# shall either be the empty text string, to indicate that the language is unknown, or a Language-Tag as defined in RFC
# 3066". RFC 3066 2.1 defines Language-Tag by a grammar (a primary subtag of 1 to 8 letters, then subtags of 1 to 8
# letters or digits), which EQX-UA1-7.2-29 tests. Whether the subtags are registered, and whether the catalog has a Lang
# at all, is asked by PAC's ValidLanguage check and by WCAG 3.1.1, not by PDF/UA-1: the IANA Language Subtag Registry
# that PAC consults belongs to BCP 47 (RFC 4646/5646) and is younger than RFC 3066, and RFC 3066 2.2 itself reserves
# "x" for private use. Those findings fail the WCAG targets and are listed for PDF/UA-1 without failing it.
LANG_BASIS = ("ISO 14289-1 7.2 + ISO 32000-1 14.9.2.2: a Lang value is a Language-Tag as defined in RFC 3066 (grammar of "
              "RFC 3066 2.1); veraPDF 7.2-29")
WCAG_LANG_FAIL = {"wcag-2.0-A": FAIL, "wcag-2.1-AA": FAIL, "wcag-2.2-AA": FAIL}

variant("lang-missing", "No natural language anywhere.", "No /Lang in the Catalog or on any element; XMP dc:title only x-default.",
        ["ISO 14289-1 7.2 (natural language shall be determined); Matterhorn 11-001 (page content), 11-002 (Alt), 11-004 "
         "(outline), 11-005 (metadata); WCAG 2.x 3.1.1 Language of Page (level A)",
         "A missing Lang is not a malformed Lang value: 7.2-29 does not apply; PAC ValidLanguage (document language missing)"],
        ["EQX-UA1-7.2-34", "EQX-UA1-7.2-22", "EQX-UA1-7.2-2", "EQX-PAC-FailValidLanguageSyntax"], doc_kw={"lang": None},
        may=["EQX-UA1-7.2-33", "EQX-UA1-7.2-24", "EQX-UA1-7.2-25", "EQX-PAC-FailAltTextHasLanguage", "EQX-UA2-8.4.4-1"],
        must_not=["EQX-UA1-7.2-29"], assessments=WCAG_LANG_FAIL)

variant("lang-invalid", "A syntactically invalid language tag.", "Catalog /Lang (en_US!) (underscore and '!' are not BCP 47).",
        [LANG_BASIS + ": '_' and '!' are outside the grammar"],
        ["EQX-UA1-7.2-29"], doc_kw={"lang": "en_US!"},
        may=["EQX-UA1-7.2-34", "EQX-UA1-7.2-22", "EQX-UA1-7.2-2", "EQX-UA1-7.2-33", "EQX-UA1-7.2-24", "EQX-UA1-7.2-25",
             "EQX-PAC-FailAltTextHasLanguage"],
        must_not=["EQX-PAC-FailValidLanguagePrimaryTagOther", "EQX-PAC-FailValidLanguageSyntax"])

variant("lang-private-use", "A private-use language tag.", "Catalog /Lang (x-corpus).",
        [LANG_BASIS + ": 'x-corpus' is a Language-Tag, and RFC 3066 2.2 reserves 'x' for private use, so PDF/UA-1 is met",
         "RFC 5646 2.2.7 (x- private use); PAC ValidLanguagePrimaryTagX; WCAG 2.x 3.1.1 (the language cannot be "
         "programmatically determined from a private tag)"],
        ["EQX-PAC-FailValidLanguagePrimaryTagX"], doc_kw={"lang": "x-corpus"}, must_not=["EQX-UA1-7.2-29"],
        assessments={"pdfua-1": AUTO_PASS, **WCAG_LANG_FAIL})

variant("lang-unknown-primary", "A well-formed tag with an unregistered primary subtag.", "Catalog /Lang (zz-ZZ).",
        [LANG_BASIS + ": 'zz-ZZ' is a Language-Tag, so PDF/UA-1 is met",
         "RFC 5646 / IANA language subtag registry ('zz' is not registered); PAC 'primary subtag is unknown'; WCAG 2.x "
         "3.1.1 (the language cannot be programmatically determined from an unassigned code)"],
        ["EQX-PAC-FailValidLanguagePrimaryTagOther"], doc_kw={"lang": "zz-ZZ"}, must_not=["EQX-UA1-7.2-29"],
        assessments={"pdfua-1": AUTO_PASS, **WCAG_LANG_FAIL})

variant("lang-valid-de", "Positive control: a valid tag other than English (de-DE).", "Catalog /Lang (de-DE).",
        ["ISO 32000-1 14.9.2; RFC 5646"], [], doc_kw={"lang": "de-DE"},
        must_not=["EQX-UA1-7.2-29", "EQX-UA1-7.2-34", "EQX-PAC-FailValidLanguagePrimaryTagOther",
                  "EQX-PAC-FailValidLanguagePrimaryTagX"], assessments={**UA1_PASS})

# --------------------------------------------------------------------------------------------------------------------------
# metadata and catalog
# --------------------------------------------------------------------------------------------------------------------------
variant("metadata-missing", "No XMP metadata stream.", "The Catalog has no /Metadata.",
        ["ISO 14289-1 7.1 paragraph 8 (Catalog shall contain Metadata); clause 5 (pdfuaid:part in XMP); Matterhorn 06-001, 07-001"],
        ["EQX-UA1-7.1-8", "EQX-UA1-5-1", "EQX-UA2-8.11.1-2"], doc_kw={"metadata": False},
        may=["EQX-UA1-7.1-9", "EQX-UA1-7.2-33", "EQX-UA2-5-1"], exclusive=("EQX-UA1-",), ua2_consequences=False,
        assessments={"pdfua-2": FAIL, "wtpdf-1.0-accessibility": FAIL, "wtpdf-1.0-reuse": FAIL})

variant("metadata-no-title", "XMP without dc:title.", "XMP has pdfuaid:part but no dc:title (the Info /Title is present).",
        ["ISO 14289-1 7.1 paragraph 8 (Metadata shall contain a dc:title); Matterhorn 06-003; WCAG 2.x 2.4.2 Page Titled"],
        ["EQX-UA1-7.1-9"], doc_kw={"xmp_title": False}, may=["EQX-UA1-7.2-33"])

variant("pdfuaid-missing", "XMP without the PDF/UA identification.", "XMP has dc:title but no pdfuaid:part.",
        ["ISO 14289-1 clause 5 (version identification in the pdfuaid schema); Matterhorn 06-002", "ISO 14289-2 clause 5"],
        ["EQX-UA1-5-1", "EQX-UA2-5-1", "EQX-WTPDF-6.1.2-1", "EQX-WTPDF-6.1.3-1"], doc_kw={"ua_part": None},
        ua2_consequences=False, may=["EQX-UA2-8.2.5.2-2"])

variant("displaydoctitle-false", "DisplayDocTitle false.", "/ViewerPreferences << /DisplayDocTitle false >>.",
        ["ISO 14289-1 7.1 paragraph 9; Matterhorn 07-002; WCAG 2.x 2.4.2"], ["EQX-UA1-7.1-10"],
        doc_kw={"display_doc_title": False}, may=["EQX-UA2-8.11.2-1"])

variant("displaydoctitle-missing", "No ViewerPreferences.", "The Catalog has no /ViewerPreferences.",
        ["ISO 14289-1 7.1 paragraph 9; Matterhorn 07-001"], ["EQX-UA1-7.1-10"], doc_kw={"display_doc_title": None},
        may=["EQX-UA2-8.11.2-1"])

# --------------------------------------------------------------------------------------------------------------------------
# links, annotations, forms
# --------------------------------------------------------------------------------------------------------------------------
def _link_body(**kw):
    def body(d: TaggedDoc):
        p = d.page()
        p.heading(1, "Links")
        p.link("example.org/a", "https://example.org/a", prefix="Read:", **kw)
    return body


variant("link-no-contents", "A Link annotation without /Contents.", "The Link annotation has no /Contents entry.",
        ["ISO 14289-1 7.18.5 (links shall contain an alternate description via Contents); 7.18.1 (annotations need Contents "
         "or Alt on the enclosing element); Matterhorn 28-004"],
        ["EQX-UA1-7.18.5-2", "EQX-UA1-7.18.1-2"], body=_link_body(contents=None),
        # the link text is the only coloured text and it is underlined (WCAG 1.4.1 / F73: a non-colour cue exists)
        must_not=["EQX-WCAG-1.4.1-UseOfColorReview"])

variant("link-not-tagged", "A Link annotation outside the structure tree.",
        "The link text is in a P, the Link annotation has no Link element, no OBJR and no /StructParent.",
        ["ISO 14289-1 7.18.5 (links shall be tagged as Link with an OBJR); ISO 14289-1 7.18.1; Matterhorn 28-011"],
        ["EQX-UA1-7.18.5-1"], body=_link_body(tagged=False), may=["EQX-UA1-7.18.1-2", "EQX-PACQ-FailMissingLinkTags",
                                                                 "EQX-PAC-FailStructuralParentTreeAnnotationHasNoStructPatent"])

variant("annots-no-tabs", "A page with annotations but no /Tabs.", "The page with a link and a field has no /Tabs entry.",
        ["ISO 14289-1 7.18.3 (Tabs /S on every page with annotations); Matterhorn 28-002"], ["EQX-UA1-7.18.3-1"],
        doc_kw={"tabs": None}, may=["EQX-UA2-8.9.3.3-1"])

variant("annots-tabs-row", "A page with annotations and /Tabs /R.", "/Tabs /R (row order) instead of /S.",
        ["ISO 14289-1 7.18.3 (Tabs shall be S); Matterhorn 28-002"], ["EQX-UA1-7.18.3-1"], doc_kw={"tabs": "R"},
        may=["EQX-UA2-8.9.3.3-1"])


def _field_body(**kw):
    def body(d: TaggedDoc):
        p = d.page()
        p.heading(1, "Form")
        p.text_field("name", "Full name", **kw)
    return body


variant("field-no-tu", "A form field without /TU and without Alt on its Form element.",
        "Text field without /TU and /Contents; its Form element has no /Alt.",
        ["ISO 14289-1 7.18.1 (a form field shall have TU, or Alt on the enclosing element); Matterhorn 28-005; WCAG 2.x 4.1.2"],
        ["EQX-UA1-7.18.1-3"], body=_field_body(tu=None, contents=None), may=["EQX-UA2-8.10.2.3-1"])

variant("field-alt-instead-of-tu", "Positive control: no /TU, but /Alt on the Form element.",
        "Text field without /TU and /Contents; its Form element has /Alt (Full name).",
        ["ISO 14289-1 7.18.1 (TU or Alt on the enclosing structure element)"], [],
        body=_field_body(tu=None, contents=None, alt_on_form="Full name"), must_not=["EQX-UA1-7.18.1-3"],
        may=["EQX-UA2-8.10.2.3-1"], assessments={**UA1_PASS})

variant("field-not-in-form", "A widget annotation outside a Form element.",
        "The text field (with /TU) has no Form element, no OBJR and no /StructParent.",
        ["ISO 14289-1 7.18.4 (widgets shall be nested in a Form tag); Matterhorn 28-010"], ["EQX-UA1-7.18.4-1"],
        body=_field_body(in_form=False), may=["EQX-UA2-8.10.1-1"])

# --------------------------------------------------------------------------------------------------------------------------
# fonts
# --------------------------------------------------------------------------------------------------------------------------
variant("font-not-embedded", "Text in a standard 14 font that is not embedded.", "F1/F2 are Helvetica and Helvetica-Bold, not embedded.",
        ["ISO 14289-1 7.21.4.1 (font programs shall be embedded); Matterhorn 31-009"], ["EQX-UA1-7.21.4.1-1"],
        doc_kw={"fonts": "standard14"}, may=["EQX-UA2-8.4.5.5.1-1"])

variant("font-type0-no-tounicode", "Composite font (Identity-H) without ToUnicode.",
        "F1 is a Type0/CIDFontType2 font with Identity-H and CIDToGIDMap /Identity, without /ToUnicode.",
        ["ISO 14289-1 7.21.7 (the font dictionary shall define the map of all used character codes to Unicode); Matterhorn 31-027"],
        ["EQX-UA1-7.21.7-1"], doc_kw={"fonts": "type0-no-tounicode"}, may=["EQX-UA2-8.4.5.8-1"])

variant("font-type0-good", "Positive control: composite font with ToUnicode.", "F1 is Type0/Identity-H with a ToUnicode CMap.",
        ["ISO 14289-1 7.21.3, 7.21.7"], [], doc_kw={"fonts": "type0"},
        must_not=["EQX-UA1-7.21.7-1", "EQX-UA1-7.21.3.1-1", "EQX-UA1-7.21.3.2-1", "EQX-UA1-7.21.5-1"], assessments={**UA1_PASS})


def _font_variant(**fkw):
    def mutate(d: TaggedDoc):
        d.fonts["F1"] = (simple_truetype(d.w, "Regular", **fkw), "simple")
    return mutate


variant("font-widths-mismatch", "Widths array inconsistent with the embedded font program.", "Every /Widths entry of F1 is 100 units too large.",
        ["ISO 14289-1 7.21.5 (glyph widths in the font dictionary and the font program shall be consistent); Matterhorn 31-012"],
        ["EQX-UA1-7.21.5-1"], mutate=_font_variant(widths_delta=100), may=["EQX-UA2-8.4.5.6-1"])

variant("font-truetype-no-encoding", "Non-symbolic TrueType font without an encoding.", "F1 has /Flags 32 (nonsymbolic) and no /Encoding.",
        ["ISO 14289-1 7.21.6 (non-symbolic TrueType shall use MacRomanEncoding or WinAnsiEncoding); Matterhorn 31-016"],
        ["EQX-UA1-7.21.6-2"], mutate=_font_variant(encoding=None), may=["EQX-UA2-8.4.5.7-2", "EQX-UA1-7.21.6-1"],
        deviations=[(r, "ISO 32000-1 9.6.6.4: a non-symbolic TrueType font without Encoding maps codes through StandardEncoding "
                     "and the Adobe Glyph List to the (3,1) cmap, which has every glyph used; veraPDF 1.30.2 also reports "
                     "missing glyphs, see CORRECTNESS.md") for r in ("EQX-UA1-7.21.4.1-2", "EQX-UA2-8.4.5.5.1-2")])

variant("font-symbolic-with-encoding", "Symbolic TrueType font with an Encoding entry.", "F1 has /Flags 4 (symbolic) and /Encoding /WinAnsiEncoding.",
        ["ISO 14289-1 7.21.6 (symbolic TrueType fonts shall not contain an Encoding entry); Matterhorn 31-017"],
        ["EQX-UA1-7.21.6-3"], mutate=_font_variant(flags=4), may=["EQX-UA2-8.4.5.7-3", "EQX-UA1-7.21.6-4", "EQX-UA2-8.4.5.7-4",
                                                       "EQX-UA1-7.21.4.1-2", "EQX-UA2-8.4.5.5.1-2"])

# --------------------------------------------------------------------------------------------------------------------------
# text contrast (WCAG 2.x 1.4.3, level AA): ratios computed with the WCAG relative-luminance formula
# --------------------------------------------------------------------------------------------------------------------------
def luminance(v8: int) -> float:
    c = v8 / 255
    return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4


def contrast_on_white(v8: int) -> float:
    return 1.05 / (luminance(v8) + 0.05)


def contrast_fixture(fid: str, grey8: int, size: float, fails: bool, purpose: str):
    ratio = contrast_on_white(grey8)
    required = 3.0 if size >= 18 else 4.5
    assert (ratio < required) == fails, (fid, ratio)

    def fn():
        d = TaggedDoc(fid)
        p = d.page()
        p.heading(1, "Contrast test")
        p.para(sentence(7, 20), size=size, color=round(grey8 / 255, 6))
        p.para(sentence(8, 20))
        aa = {t: (FAIL if fails else AUTO_PASS) for t in ("wcag-2.0-AA", "wcag-2.1-AA", "wcag-2.2-AA")}
        aa.update({t: AUTO_PASS for t in ("wcag-2.0-A", "wcag-2.1-A", "wcag-2.2-A")} if fails else {})
        rule = "EQX-WCAG-1.4.3-TextContrast"
        return d.build(), spec(
            fid, purpose, f"Tagged one-page document (H1 + 2 P, embedded fonts) on a white page; the first P is {size:g} pt "
            f"regular text in DeviceGray {grey8}/255 (#{grey8:02x}{grey8:02x}{grey8:02x}), contrast {ratio:.3f}:1 against white "
            f"(WCAG relative luminance), required {required:g}:1 for {'large' if size >= 18 else 'normal'} text.",
            ["WCAG 2.x SC 1.4.3 Contrast (Minimum), level AA: 4.5:1, 3:1 for large text (18 pt, or 14 pt bold)"],
            must_report=[rule] if fails else [], must_not_report=[] if fails else [rule, "EQX-WCAG-1.4.3-TextContrastReview"],
            may_report=["EQX-PAC-FailNotEnoughContrastForText"] if fails else [],
            exclusive=("EQX-UA1-", "EQX-WCAG-"), assessments={**({} if fails else UA1_PASS), **aa})
    fn.__name__ = fid
    FIXTURE_FNS.append(fn)


contrast_fixture("contrast-black", 0, 11, False, "Positive control: black 11 pt text on white (21:1).")
contrast_fixture("contrast-lightgrey", 0xCC, 11, True, "Light grey 11 pt text on white (1.6:1), clearly below 4.5:1.")
contrast_fixture("contrast-boundary-pass", 0x76, 11, False, "11 pt #767676 on white (4.54:1), just above 4.5:1.")
contrast_fixture("contrast-boundary-fail", 0x77, 11, True, "11 pt #777777 on white (4.48:1), just below 4.5:1.")
contrast_fixture("contrast-large-pass", 0x94, 18, False, "18 pt (large) #949494 on white (3.03:1), above the 3:1 large-text minimum.")
contrast_fixture("contrast-large-fail", 0xA0, 18, True, "18 pt (large) #a0a0a0 on white (2.67:1), below 3:1.")


# --------------------------------------------------------------------------------------------------------------------------
# scanned / image-only
# --------------------------------------------------------------------------------------------------------------------------
@fixture
def scanned_untagged():
    fid = "scanned-untagged"
    d = TaggedDoc(fid, tagged=False, mark_info={}, outline=False)
    p = d.page(furniture=False)
    p.image("Im1", scanned_image(d.w), 0, 0, PAGE_W, PAGE_H)
    return d.build(), spec(
        fid, "Image-only page (a 'scan'): no text objects, no structure.",
        "One page painted by a 240x320 DeviceGray image that resembles text lines; no text, no StructTreeRoot, no MarkInfo.",
        ["ISO 14289-1 7.1 (no structure tree; content neither tagged nor artifact); Matterhorn 01-005, 01-006",
         "WCAG 2.x 1.1.1 / 1.4.5 (images of text; manual)"],
        must_report=["EQX-UA1-7.1-11", "EQX-UA1-6.2-1", "EQX-UA1-7.1-3"], assessments={"pdfua-1": FAIL}, tagged=False,
        ua2_consequences=False)


@fixture
def scanned_tagged_figure():
    fid = "scanned-tagged-figure"
    d = TaggedDoc(fid, outline=False)
    p = d.page(furniture=False)
    se = d.elem("Figure")
    se.extra["Alt"] = "Scanned page of a letter (text not available)"
    se.extra["A"] = {"O": Name("Layout"), "Placement": Name("Block"), "BBox": [0, 0, PAGE_W, PAGE_H]}
    p.image("Im1", scanned_image(d.w), 0, 0, PAGE_W, PAGE_H, se=se)
    return d.build(), spec(
        fid, "Scope limit: a scanned page tagged as one Figure with Alt meets the machine-checkable PDF/UA-1 conditions, "
        "although the text itself is only an image (a manual check, WCAG 1.4.5 / PDF/UA 7.3, is needed to reject it).",
        "One page painted by an image inside a Figure with /Alt and BBox; Lang, XMP, MarkInfo, DisplayDocTitle present.",
        ["ISO 14289-1 7.1, 7.3 (machine-checkable parts satisfied)", "WCAG 2.x 1.4.5 Images of Text (not machine-checkable)"],
        exclusive=("EQX-UA1-",), may_report=["EQX-PACQ-WarnDocumentHasHeadings", "EQX-WCAG-1.4.11-NonTextContrastReview"],
        assessments={**UA1_PASS})


# --------------------------------------------------------------------------------------------------------------------------
# encryption
# --------------------------------------------------------------------------------------------------------------------------
@fixture
def encrypted_user_password():
    fid = "encrypted-user-password"
    d = good_doc(fid)
    d.w.enable_rc4(b"corpus-secret", b"corpus-owner", -4)
    return d.build(), spec(
        fid, "Encrypted with a user (open) password: cannot be opened without it.",
        "ua1-good encrypted with the standard security handler, RC4 128-bit (V 2, R 3), user password 'corpus-secret', owner "
        "password 'corpus-owner', /P -4.",
        ["ISO 32000-1 7.6.3 (a user password is needed to decrypt); the validator documents 'not processed' for such files"],
        exit_code=2, result="not_processed", processed=False, assessments=NOT_PROCESSED, tagged=None)


@fixture
def encrypted_owner_only():
    fid = "encrypted-owner-only"
    d = good_doc(fid)
    d.w.enable_rc4(b"", b"corpus-owner", -4)
    return d.build(), spec(
        fid, "Encrypted with an owner password only (empty user password): opens without a password.",
        "ua1-good encrypted with RC4 128-bit (V 2, R 3), empty user password, owner password 'corpus-owner', /P -4 (all "
        "permissions, including bit 10 'extract for accessibility').",
        ["ISO 32000-1 7.6.3.4 (empty user password opens the file)", "ISO 14289-1 7.16 (P key present; bit 10 set)"],
        must_not_report=["EQX-UA1-7.16-1"], exclusive=("EQX-UA1-", "EQX-PAC-Fail", "EQX-PACQ-Fail", "EQX-WCAG-"),
        assessments={**UA1_PASS})


# --------------------------------------------------------------------------------------------------------------------------
# malformed and edge inputs
# --------------------------------------------------------------------------------------------------------------------------
def malformed(fid: str, purpose: str, construction: str, data_fn, *, exit_code=2, result=("not_processed",), basis=None,
              processed=False):
    def fn():
        return data_fn(), spec(fid, purpose, construction, basis or ["ISO 32000-1 7.5 (file structure)"], exit_code=exit_code,
                               result=list(result), processed=processed,
                               assessments=NOT_PROCESSED if processed is False else {}, tagged=None, ua2_consequences=False)
    fn.__name__ = fid
    FIXTURE_FNS.append(fn)


def _good_bytes(seed: str) -> bytes:
    return good_doc(seed).build()


def _bad_xref() -> bytes:
    data = bytearray(_good_bytes("malformed-bad-xref"))
    xref = data.rindex(b"\nxref\n") + 1
    head = data[:xref]
    tail = data[xref:].decode("latin-1").split("\n")
    out = []
    for ln in tail:
        if len(ln) == 19 and ln.endswith(" n ") and ln[:10].isdigit():
            ln = "%010d 00000 n " % (int(ln[:10]) + 7)
        out.append(ln)
    return bytes(head) + "\n".join(out).encode("latin-1")


def _garbage() -> bytes:
    x, out = 12345, bytearray()
    for _ in range(4096):
        x = (x * 6364136223846793005 + 1442695040888963407) % 2**64
        out.append(x >> 56)
    return bytes(out)


def _no_root() -> bytes:
    data = _good_bytes("malformed-no-root")
    i = data.rindex(b"trailer\n")
    tail = data[i:].replace(b"/Root", b"/Xoot")
    return data[:i] + tail


def _truncated() -> bytes:
    data = _good_bytes("malformed-truncated")
    return data[: len(data) * 6 // 10]


malformed("malformed-zero-byte", "Empty file.", "0 bytes.", lambda: b"")
malformed("malformed-not-pdf", "A text file with a .pdf name.", "ASCII text, no %PDF header.",
          lambda: b"This is not a PDF file.\nIt is plain text.\n")
malformed("malformed-garbage", "4 KiB of pseudo-random bytes.", "Deterministic LCG output, no PDF syntax.", _garbage)
malformed("malformed-header-only", "Only a header and an EOF marker.", "%PDF-1.7 + %%EOF, no objects, no trailer.",
          lambda: b"%PDF-1.7\n%%EOF\n")
malformed("malformed-no-root", "Trailer without /Root.", "ua1-good with the trailer key /Root renamed to /Xoot.", _no_root,
          exit_code=2, result=("not_processed", "incomplete"), processed=None)
malformed("malformed-truncated", "A file cut off at 60 % of its length (no xref, no trailer).",
          "The first 60 % of the bytes of ua1-good.", _truncated, exit_code=2, result=("not_processed", "incomplete"),
          processed=None, basis=["ISO 32000-1 7.5 (a file without cross-reference table and trailer is damaged; a repaired "
                                 "reading is not the file, so no rule may pass)"])
malformed("malformed-bad-xref", "Cross-reference offsets all wrong by 7 bytes.",
          "ua1-good with every in-use xref entry offset increased by 7.", _bad_xref, exit_code=2,
          result=("incomplete",), processed=True,
          basis=["ISO 32000-1 7.5.4 (xref offsets locate the objects); dox-validate's documented policy: a file qpdf had to repair "
                 "is reported with every rule incomplete (exit 2), never as a pass"])


@fixture
def edge_zero_pages():
    fid = "edge-zero-pages"
    w = Writer("1.7", fid)
    pages = w.add({"Type": Name("Pages"), "Kids": [], "Count": 0})
    root = w.add({"Type": Name("Catalog"), "Pages": pages})
    return w.write(root), spec(
        fid, "A syntactically valid file with an empty page tree (no structure, no metadata).",
        "Catalog + Pages with /Count 0 and no kids; nothing else.",
        ["ISO 14289-1 7.1 (structure tree), 7.1 paragraph 8 (Metadata), 6.2 (MarkInfo)"],
        exit_code=[1, 2], result=["errors_found", "incomplete", "not_processed"], processed=None,
        exclusive=(), tagged=None, ua2_consequences=False)


@fixture
def edge_empty_tagged_page():
    fid = "edge-empty-tagged-page"
    d = TaggedDoc(fid, outline=False)
    d.page(furniture=False)
    return d.build(), spec(
        fid, "A tagged document whose only page has an empty content stream and an empty Document element.",
        "Catalog with all PDF/UA-1 metadata, StructTreeRoot -> Document with no kids, one page with empty content.",
        ["ISO 14289-1 clause 7 (nothing to tag: no content requirement is violated)"],
        exclusive=("EQX-UA1-",), may_report=["EQX-PACQ-WarnDocumentHasHeadings"], assessments={**UA1_PASS})


@fixture
def edge_deep_structure():
    fid = "edge-deep-structure"
    d = TaggedDoc(fid, outline=False)
    p = d.page()
    parent = d.top
    for _ in range(400):
        parent = d.elem("Div", parent)
    p.para("Text at nesting depth 401.", parent)
    return d.build(), spec(
        fid, "Robustness: a structure tree 400 Div elements deep.", "Document > 400 nested Div > P with one text line.",
        ["ISO 32000-1 14.7.2 (no depth limit); the validator must terminate without a crash"],
        exclusive=(), exit_code=[1, 2], result=["errors_found", "incomplete"], may_report=[],
        assessments={}, ua2_consequences=False,
        # nesting violates no PDF/UA-1 or WCAG requirement; a reader may stop at a depth limit (incomplete), never fail
        auto=([0, 2], ["no_errors_found", "incomplete"]))


@fixture
def edge_incremental_update():
    fid = "edge-incremental-update"
    d = good_doc(fid)
    data = d.build()
    # the catalog is the last object written; replace it with a revision that has no /Lang
    xref_at = int(data[data.rindex(b"startxref\n") + 10:].split(b"\n")[0])
    cat_num = max(n for n, o in d.w.objects.items() if isinstance(o, dict) and o.get("Type") == "Catalog")
    cat = dict(d.w.objects[cat_num])
    cat.pop("Lang")
    obj = b"%d 0 obj\n" % cat_num + d.w.ser(cat) + b"\nendobj\n"
    off = len(data)
    size = len(d.w.order) + 1
    info_num = max(n for n, o in d.w.objects.items() if isinstance(o, dict) and "Producer" in o and "Title" in o)
    trailer = d.w.ser({"Size": size, "Root": Raw(f"{cat_num} 0 R"), "Info": Raw(f"{info_num} 0 R"), "Prev": xref_at,
                       "ID": [Str(d.w.id0, hex=True), Str(d.w.id0, hex=True)]})
    upd = obj + b"xref\n0 1\n0000000000 65535 f \n%d 1\n%010d 00000 n \ntrailer\n" % (cat_num, off) + trailer
    xref2 = off + len(obj)
    upd += b"\nstartxref\n%d\n%%%%EOF\n" % xref2
    return data + upd, spec(
        fid, "An incremental update that removes /Lang from the Catalog: the newest revision counts.",
        "ua1-good followed by an incremental update (new xref section with /Prev) that rewrites the Catalog without /Lang.",
        ["ISO 32000-1 7.5.6 (incremental updates: the latest revision of an object is the valid one)",
         "ISO 14289-1 7.2; Matterhorn 11-001"],
        must_report=["EQX-UA1-7.2-34"],
        may_report=["EQX-UA1-7.2-22", "EQX-UA1-7.2-2", "EQX-UA1-7.2-33", "EQX-UA1-7.2-24", "EQX-UA1-7.2-25",
                    "EQX-UA1-7.2-29", "EQX-PAC-FailAltTextHasLanguage", "EQX-UA2-8.4.4-1"],
        assessments={"pdfua-1": FAIL})


# --------------------------------------------------------------------------------------------------------------------------
# further catalogue-level and annotation requirements
# --------------------------------------------------------------------------------------------------------------------------
def _headers_ids(d: TaggedDoc):
    p = d.page()
    p.heading(1, "Table with Headers and IDs")
    t = p.table(ROWS, scope=False)
    rows = [k for k in t.kids if isinstance(k, SE)]
    ids = []
    for ci, th in enumerate(k for k in rows[0].kids if isinstance(k, SE)):
        th.extra["ID"] = Str(b"h%d" % ci)
        ids.append(Str(b"h%d" % ci))
    for tr in rows[1:]:
        for ci, td in enumerate(k for k in tr.kids if isinstance(k, SE)):
            td.extra["A"] = {"O": Name("Table"), "Headers": [ids[ci]]}


variant("table-headers-ids", "Positive control: header association by Headers/IDs instead of Scope.",
        "3x3 table, TH with /ID and no /Scope, every TD with /A << /O /Table /Headers [(h<col>)] >>.",
        ["ISO 14289-1 7.5 (Scope is required only when the structure is not determinable via Headers and IDs)"], [],
        body=_headers_ids, must_not=["EQX-UA1-7.5-1", "EQX-UA1-7.5-2"], assessments={**UA1_PASS})


def _text_annot(d: TaggedDoc):
    p = d.pages[0]
    p.annots.append(Annot(p.index, {"Type": Name("Annot"), "Subtype": Name("Text"), "Rect": [500, 700, 520, 720],
                                    "Contents": "Reviewer note", "F": 4}, None, struct_parent=False))


variant("annot-text-not-tagged", "A markup (Text) annotation outside the structure tree.",
        "A /Text annotation with /Contents, no Annot element, no /StructParent.",
        ["ISO 14289-1 7.18.1 (annotations other than Widget, PrinterMark and Link shall be nested in an Annot tag); Matterhorn 28-001"],
        ["EQX-UA1-7.18.1-1"], mutate=_text_annot, may=["EQX-UA2-8.9.2.3-1", "EQX-PAC-FailStructuralParentTreeAnnotationHasNoStructPatent"])


def _trapnet(d: TaggedDoc):
    p = d.pages[0]
    p.annots.append(Annot(p.index, {"Type": Name("Annot"), "Subtype": Name("TrapNet"), "Rect": [100, 100, 200, 200],
                                    "LastModified": Str(b"D:20240101000000Z"), "F": 4}, None, struct_parent=False))


variant("annot-trapnet", "A TrapNet annotation.", "A /TrapNet annotation with /Rect [100 100 200 200] (inside the page).",
        ["ISO 14289-1 7.18.2 (TrapNet annotations shall not be permitted); Matterhorn 28-003"], ["EQX-UA1-7.18.2-1"],
        mutate=_trapnet, may=["EQX-UA1-7.18.1-1", "EQX-UA1-7.18.1-2", "EQX-UA2-8.9.2.4.15-1", "EQX-UA2-8.9.2.3-1",
                              "EQX-PAC-FailStructuralParentTreeAnnotationHasNoStructPatent", "EQX-PAC-FailAnnotationHasAltText"])


def _oc_no_name(d: TaggedDoc):
    ocg = d.w.add({"Type": Name("OCG"), "Name": "Layer 1"})
    d.catalog_extra["OCProperties"] = {"OCGs": [ocg], "D": {"Order": [ocg], "ON": [ocg]}}


variant("oc-config-no-name", "Optional content configuration without /Name.", "/OCProperties /D without a /Name entry.",
        ["ISO 14289-1 7.10 (each optional content configuration dictionary shall contain Name); Matterhorn 20-001"],
        ["EQX-UA1-7.10-1"], mutate=_oc_no_name, may=["EQX-UA2-8.7-1"])


def _embedded_no_name(d: TaggedDoc):
    ef = d.w.add(Stream({"Type": Name("EmbeddedFile")}, b"attachment"))
    fs = d.w.add({"Type": Name("Filespec"), "EF": {"F": ef}})
    d.catalog_extra["Names"] = {"EmbeddedFiles": {"Names": [Str(b"attachment"), fs]}}


variant("embedded-file-no-name", "Embedded file specification without /F and /UF.", "EmbeddedFiles name tree with a Filespec lacking F and UF.",
        ["ISO 14289-1 7.11 (file specification dictionaries for embedded files shall contain F and UF); Matterhorn 21-001"],
        ["EQX-UA1-7.11-1"], mutate=_embedded_no_name, may=["EQX-UA2-8.14.1-1"])


XFA_DYNAMIC = (b'<?xml version="1.0" encoding="UTF-8"?>\n<xdp:xdp xmlns:xdp="http://ns.adobe.com/xdp/">'
               b'<config xmlns="http://www.xfa.org/schema/xci/3.0/"><acrobat><acrobat7><dynamicRender>required</dynamicRender>'
               b'</acrobat7></acrobat></config></xdp:xdp>\n')


def _xfa(d: TaggedDoc):
    def hook(doc, ctx):
        ctx["catalog"]["AcroForm"]["XFA"] = doc.w.add(Stream({}, XFA_DYNAMIC))
        ctx["catalog"]["NeedsRendering"] = True
    d.hooks.append(hook)


variant("xfa-dynamic", "Dynamic XFA form.", "AcroForm /XFA stream whose config packet sets acrobat7/dynamicRender to 'required', and Catalog /NeedsRendering true.",
        ["ISO 14289-1 7.15 (no dynamic XFA forms); Matterhorn 25-001"], ["EQX-UA1-7.15-1"],
        mutate=_xfa, may=["EQX-UA2-8.10.1-3"])


# --------------------------------------------------------------------------------------------------------------------------
# on-demand size tiers (not committed)
# --------------------------------------------------------------------------------------------------------------------------
def big_simple(fid: str, pages: int) -> bytes:
    d = TaggedDoc(fid, tagged=False, mark_info={}, outline=False)
    for i in range(pages):
        p = d.page(furniture=False)
        p.untagged_text(f"Section {i + 1}", size=14)
        for k in range(40):
            p.untagged_text(sentence(i * 41 + k, 12), size=10)
    return d.build()


def big_complex(fid: str, pages: int) -> bytes:
    d = TaggedDoc(fid)
    sect = None
    for i in range(pages):
        p = d.page()
        if i % 10 == 0:
            sect = d.elem("Sect")
            p.heading(1, f"Part {i // 10 + 1}", sect)
        p.heading(2, f"Section {i + 1}", sect)
        for k in range(3):
            p.para(sentence(i * 7 + k, 26), sect, size=10)
        if i % 3 == 0:
            p.table([["Item", "Count", "Share"]] + [[f"Row {r}", str(r * 3), f"{r}%"] for r in range(1, 6)], sect)
        elif i % 3 == 1:
            p.list([sentence(i + r, 6) for r in range(5)], sect)
        else:
            p.figure(f"Chart {i + 1}", sect, h=80)
        p.link(f"example.org/s{i}", f"https://example.org/s{i}", sect, prefix="Source:")
        p.para(sentence(i * 7 + 5, 22), sect, size=10)
    return d.build()


GENERATED = [
    # id, pages, complexity
    ("gen-medium-20-simple", 20, "simple"),
    ("gen-medium-100-complex", 100, "complex"),
    ("gen-large-500-simple", 500, "simple"),
    ("gen-large-500-complex", 500, "complex"),
    ("gen-large-2000-simple", 2000, "simple"),
    ("gen-large-2000-complex", 2000, "complex"),
]


def generated_spec(fid: str, pages: int, complexity: str) -> dict:
    tier = "medium" if pages <= 100 else "large"
    if complexity == "simple":
        return spec(fid, f"Size tier {tier}: {pages} untagged text pages (performance, simple).",
                    f"{pages} pages, each 41 lines of text in an embedded font, no structure tree, no MarkInfo.",
                    ["ISO 14289-1 7.1 (no structure tree)"], must_report=["EQX-UA1-7.1-11", "EQX-UA1-6.2-1", "EQX-UA1-7.1-3"],
                    assessments={"pdfua-1": FAIL}, tagged=False, ua2_consequences=False, category=tier) | {"pages": pages, "complexity": complexity}
    return spec(fid, f"Size tier {tier}: {pages} tagged pages with a rich structure tree (performance, complex).",
                f"{pages} pages; per page H2, 4 P, a Link (OBJR, /Contents) and a 6x3 Table, a 5-item list or a Figure with Alt; "
                "Sect + H1 every 10 pages; outline entries for all headings; otherwise as ua1-good.",
                ["ISO 14289-1 clauses 5-7 (all machine-checkable conditions satisfied by construction)"],
                exclusive=("EQX-UA1-", "EQX-PAC-Fail", "EQX-PACQ-Fail", "EQX-WCAG-"), assessments={**UA1_PASS},
                category=tier) | {"pages": pages, "complexity": complexity}


# --------------------------------------------------------------------------------------------------------------------------
def build_fixtures(out: Path) -> dict:
    out.mkdir(parents=True, exist_ok=True)
    entries = []
    for fn in FIXTURE_FNS:
        data, sp = fn()
        (out / sp["file"]).write_bytes(data)
        sp["sha256"] = hashlib.sha256(data).hexdigest()
        sp["bytes"] = len(data)
        entries.append(sp)
    ids = [e["id"] for e in entries]
    assert len(ids) == len(set(ids)), "duplicate fixture id"
    doc = {"schema": "dox-validate-corpus-expectations/1",
           "note": "Expectations derive from the construction of each file and the cited requirement, not from any tool output. "
                   "must_report: rule ids that shall be reported; must_not_report: rule ids that shall not be reported; "
                   "exclusive_prefixes: every reported rule with one of these prefixes shall be in must_report or may_report; "
                   "may_report: consequential findings that are allowed but not required; exit_code/result: allowed values; "
                   "assessments: expected status of the named targets.",
           "fixtures": entries}
    (out / "expected.json").write_text(json.dumps(doc, indent=1, ensure_ascii=False) + "\n")
    return doc


def build_generated(out: Path, only: list[str] | None = None) -> dict:
    out.mkdir(parents=True, exist_ok=True)
    entries = []
    for fid, pages, cx in GENERATED:
        if only and fid not in only:
            continue
        data = (big_simple if cx == "simple" else big_complex)(fid, pages)
        (out / f"{fid}.pdf").write_bytes(data)
        sp = generated_spec(fid, pages, cx)
        sp["sha256"], sp["bytes"] = hashlib.sha256(data).hexdigest(), len(data)
        entries.append(sp)
        print(f"{fid}: {len(data)} bytes", file=sys.stderr)
    doc = {"schema": "dox-validate-corpus-expectations/1", "fixtures": entries}
    if not only:
        (out / "generated.json").write_text(json.dumps(doc, indent=1) + "\n")
    return doc


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="regenerate into a temporary directory and compare")
    ap.add_argument("--large", action="store_true", help="write the on-demand medium/large tiers")
    ap.add_argument("--out", type=Path)
    ap.add_argument("--only", nargs="*")
    a = ap.parse_args()
    if a.large:
        build_generated(a.out or CACHE, a.only)
        return 0
    if a.check:
        with tempfile.TemporaryDirectory() as t:
            build_fixtures(Path(t))
            bad = []
            for f in sorted(Path(t).iterdir()):
                ref = FIXTURES / f.name
                if not ref.exists() or ref.read_bytes() != f.read_bytes():
                    bad.append(f.name)
            extra = sorted(set(p.name for p in FIXTURES.iterdir()) - set(p.name for p in Path(t).iterdir()))
            for name in bad:
                print(f"differs or missing: {name}")
            for name in extra:
                print(f"not generated (stale): {name}")
            print("fixtures reproduce exactly" if not bad and not extra else "MISMATCH")
            return 1 if bad or extra else 0
    doc = build_fixtures(a.out or FIXTURES)
    total = sum(e["bytes"] for e in doc["fixtures"])
    print(f"{len(doc['fixtures'])} fixtures, {total} bytes", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
