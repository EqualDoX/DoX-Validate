"""WCAG 2.x SC 1.4.3 (Contrast, Minimum) cases built on the fly with the corpus generator (not part of the frozen corpus).

Ratios are computed here with the WCAG relative-luminance formula; the expected verdict follows from the success
criterion: 4.5:1 for normal text, 3:1 for large text (at least 18 pt, or 14 pt bold). Incidental text (pure decoration,
invisible text) has no requirement.
"""
import pytest

from conftest import run
from generate import contrast_on_white, luminance, sentence
from pdfgen import MARGIN, TaggedDoc, fmt_num

RULE, REVIEW = "EQX-WCAG-1.4.3-TextContrast", "EQX-WCAG-1.4.3-TextContrastReview"


def ratio(fg8: int, bg8: int) -> float:
    a, b = sorted((luminance(fg8), luminance(bg8)), reverse=True)
    return (a + 0.05) / (b + 0.05)


def build(tmp_path, name, *, grey8, size, bold=False, bg8=None, tr=0, artifact=False):
    d = TaggedDoc(name)
    p = d.page()
    p.heading(1, "Contrast case")
    font = "F2" if bold else "F1"
    p.y -= size * 1.5
    body = p.text_ops(sentence(3, 8), MARGIN, p.y, size, font, color=round(grey8 / 255, 6), tr=tr)
    if bg8 is not None:   # an opaque rectangle behind the text, as page decoration (artifact)
        p.artifact(b"q %s g %d %d 468 %s re f Q\n" % (fmt_num(round(bg8 / 255, 6)).encode(), MARGIN - 4, int(p.y - size * 0.4),
                                                    fmt_num(size * 1.6).encode()), "Layout")
    if artifact:
        p.artifact(body, "Pagination")
    else:
        p._mc(d.elem("P"), body)
    p.para(sentence(4, 20))
    f = tmp_path / f"{name}.pdf"
    f.write_bytes(d.build())
    rep = run("--format", "json", f).json()
    return {x["rule"] for x in rep["findings"]}, rep


CASES = [
    # name, grey, size, bold, background, expected violation
    ("bold14-large-pass", 0x94, 14, True, None, False),     # 3.03:1, 14 pt bold is large text
    ("regular14-fail", 0x94, 14, False, None, True),        # same colour, 14 pt regular is normal text
    ("bold13-fail", 0x94, 13, True, None, True),            # bold below 14 pt is normal text
    ("white-on-dark-pass", 0xFF, 11, False, 0x33, False),   # white on #333333: 12.6:1
    ("white-on-grey-fail", 0xFF, 11, False, 0x77, True),    # white on #777777: 4.48:1
    ("dark-on-light-pass", 0x00, 11, False, 0xDD, False),   # black on #dddddd: 15.4:1
]


@pytest.mark.parametrize("name,grey,size,bold,bg,fails", CASES, ids=[c[0] for c in CASES])
def test_contrast_case(tmp_path, name, grey, size, bold, bg, fails):
    r = ratio(grey, 0xFF if bg is None else bg)
    large = size >= 18 or (size >= 14 and bold)
    assert (r < (3.0 if large else 4.5)) == fails, "case table inconsistent with WCAG 1.4.3"
    rules, _ = build(tmp_path, name, grey8=grey, size=size, bold=bold, bg8=bg)
    if fails:
        assert RULE in rules, f"contrast {r:.2f}:1 not reported"
    else:
        assert RULE not in rules and REVIEW not in rules, f"contrast {r:.2f}:1 reported"


def test_invisible_text_is_exempt(tmp_path):
    rules, _ = build(tmp_path, "invisible", grey8=0xEE, size=11, tr=3)   # Tr 3: neither filled nor stroked
    assert RULE not in rules and REVIEW not in rules


def test_artifact_text_is_not_a_confirmed_violation(tmp_path):
    """Light page furniture marked as Artifact: WCAG exempts pure decoration, so at most a review item."""
    assert contrast_on_white(0xCC) < 4.5
    rules, _ = build(tmp_path, "artifact", grey8=0xCC, size=11, artifact=True)
    assert RULE not in rules
