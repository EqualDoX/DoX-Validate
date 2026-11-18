"""Form-field cases built on the fly with the corpus generator (not part of the frozen corpus).

ISO 14289-1 7.18.1 asks for the name of a form field: a TU entry, or an Alt on the enclosing structure element
(`EQX-UA1-7.18.1-3`, Matterhorn 28-005). Clause 7.18 does not apply to annotations whose Hidden flag is set or whose
rectangle is outside the crop box (Matterhorn Protocol 1.1, checkpoint 28).

A rectangle without area needs care. veraPDF's test counts it as outside the crop box whenever it touches the edge of
the box, so a field at [0 0 0 0] (an invisible signature of an office suite) passes there; PAC fails such a field, and
so does dox-validate (a `PAC:` occurrence). The original observation is the other side of it: a field without area
that lies beyond the crop box - veraPDF's 7.18.1-t03-pass-d.pdf, /Rect [800 800 800 800] on a 612 x 792 page - was
failed as well, by PAC and by dox-validate, although the annotation is outside the crop box."""
import pytest

from conftest import run
from pdfgen import TaggedDoc

RULE = "EQX-UA1-7.18.1-3"
PAGE = [0, 0, 612, 792]


def field_doc(rect, *, tu=None, flags=4, crop=None, alt_on_form=None) -> TaggedDoc:
    d = TaggedDoc("field-case")
    p = d.page()
    p.heading(1, "Form")
    p.text_field("name", "Full name", tu=tu, contents=None, alt_on_form=alt_on_form)
    a = p.annots[-1].dict
    a["Rect"] = rect
    a["F"] = flags
    if crop is not None:
        p.extra["CropBox"] = crop
    return d


def finding(tmp_path, d):
    f = tmp_path / "field.pdf"
    f.write_bytes(d.build())
    rep = run("--format", "json", f).json()
    return next((x for x in rep["findings"] if x["rule"] == RULE), None), rep


# rect, crop box (None: the 612 x 792 MediaBox), reported?
NO_NAME_CASES = [
    # a visible field without a name: the plain violation
    ("visible", [72, 600, 272, 622], None, True),
    # no area, on the page: not outside the crop box, not hidden
    ("zero-area-origin", [0, 0, 0, 0], None, True),
    ("zero-area-inside", [100, 100, 100, 100], None, True),
    ("zero-area-far-corner", [612, 792, 612, 792], None, True),
    ("zero-area-on-edge", [612, 300, 612, 300], None, True),
    # no area, beyond the crop box: outside it (veraPDF 7.18.1-t03-pass-d)
    ("zero-area-outside", [800, 800, 800, 800], None, False),
    ("zero-area-just-outside-right", [612.5, 300, 612.5, 300], None, False),
    ("zero-area-just-outside-top", [300, 792.5, 300, 792.5], None, False),
    ("zero-area-negative", [-1, -1, -1, -1], None, False),
    # the crop box decides, not the media box
    ("zero-area-outside-cropbox", [50, 50, 50, 50], [100, 100, 500, 700], False),
    ("zero-area-on-cropbox-corner", [100, 100, 100, 100], [100, 100, 500, 700], True),
    ("zero-area-inside-cropbox", [300, 300, 300, 300], [100, 100, 500, 700], True),
    # with an area and outside the crop box: exempt, as before
    ("area-outside", [800, 800, 850, 820], None, False),
    ("area-outside-cropbox", [10, 10, 60, 30], [100, 100, 500, 700], False),
]


@pytest.mark.parametrize("name,rect,crop,reported", NO_NAME_CASES, ids=[c[0] for c in NO_NAME_CASES])
def test_field_without_tu_and_alt(tmp_path, name, rect, crop, reported):
    f, rep = finding(tmp_path, field_doc(rect, crop=crop))
    assert (f is not None) == reported, f
    if reported:
        assert f["severity"] == "error"
        assert rep["assessments"]["pdfua-1"]["status"] == "fail" and RULE in rep["assessments"]["pdfua-1"]["failed_rules"]
    else:
        assert RULE not in rep["assessments"]["pdfua-1"]["failed_rules"]


def test_zero_area_field_on_the_page_is_reported_through_pacs_definition(tmp_path):
    f, _ = finding(tmp_path, field_doc([0, 0, 0, 0]))
    assert f["occurrences"] == 1 and f["non_pac_occurrences"] == 0
    assert f["locations"][0]["detail"].startswith("PAC:")


@pytest.mark.parametrize("rect", [[0, 0, 0, 0], [100, 100, 100, 100], [72, 600, 272, 622]], ids=["origin", "inside", "visible"])
def test_named_field_is_not_reported(tmp_path, rect):
    assert finding(tmp_path, field_doc(rect, tu="Full name"))[0] is None
    assert finding(tmp_path, field_doc(rect, alt_on_form="Full name"))[0] is None


@pytest.mark.parametrize("rect", [[0, 0, 0, 0], [100, 100, 100, 100], [72, 600, 272, 622]], ids=["origin", "inside", "visible"])
def test_hidden_field_is_not_reported(tmp_path, rect):
    assert finding(tmp_path, field_doc(rect, flags=4 | 2))[0] is None
