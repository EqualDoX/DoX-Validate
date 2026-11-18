"""Document-language cases built on the fly with the corpus generator (not part of the frozen corpus).

What ISO 14289-1 asks of a Lang value and what PAC's ValidLanguage check asks are two requirements:

- ISO 14289-1 7.2 refers to ISO 32000-1 14.9.2. Its 14.9.2.2 says: "A language identifier shall either be the empty
  text string, to indicate that the language is unknown, or a Language-Tag as defined in RFC 3066". RFC 3066 2.1
  defines Language-Tag as Primary-subtag *( "-" Subtag ), Primary-subtag = 1*8ALPHA, Subtag = 1*8(ALPHA / DIGIT).
  That grammar is `EQX-UA1-7.2-29` (and `EQX-UA2-8.4.4-2`). No clause asks for a catalog Lang in PDF/UA-1 as long as
  the language of every text can be determined.
- PAC also wants a document language, a well-formed BCP 47 tag and a primary subtag of the IANA Language Subtag
  Registry (which is younger than RFC 3066). Those findings are `EQX-PAC-FailValidLanguageSyntax`,
  `EQX-PAC-FailValidLanguagePrimaryTagOther` and `EQX-PAC-FailValidLanguagePrimaryTagX`: error findings that fail
  WCAG 3.1.1 and are listed for PDF/UA-1 without failing it.

The original observations: real-world documents and a file of the PDF/UA Reference Suite with /Lang (Deutsch) or
(English), and veraPDF's 7.2-t29 pass files ('portugue', 'p', ...), were reported as 7.2-29 "syntax errors"."""
import pytest

from conftest import run
from pdfgen import Name, Str, Stream, TaggedDoc, xmp

GRAMMAR = "EQX-UA1-7.2-29"            # ISO 32000-1 14.9.2.2 / RFC 3066 grammar, PDF/UA-1
GRAMMAR_UA2 = "EQX-UA2-8.4.4-2"       # the same test for PDF/UA-2 and WTPDF
PAC_SYNTAX = "EQX-PAC-FailValidLanguageSyntax"
PAC_OTHER = "EQX-PAC-FailValidLanguagePrimaryTagOther"
PAC_X = "EQX-PAC-FailValidLanguagePrimaryTagX"
PAC_I = "EQX-PAC-WarnValidLanguagePrimaryTagI"
LANGUAGE_RULES = {GRAMMAR, GRAMMAR_UA2, PAC_SYNTAX, PAC_OTHER, PAC_X, PAC_I}
FAIL, PASS = "fail", "automated_pass_manual_required"


def body(d: TaggedDoc, furniture: bool = True) -> None:
    p = d.page(furniture=furniture)
    p.heading(1, "Language")
    p.para("A paragraph of text in the document language.")
    p.figure("A figure with alternative text")


def report(tmp_path, d: TaggedDoc, name: str) -> dict:
    f = tmp_path / f"{name}.pdf"
    f.write_bytes(d.build())
    r = run("--format", "json", f)
    assert r.code in (0, 1), r.stderr
    return r.json()


def by_rule(rep: dict) -> dict:
    return {x["rule"]: x for x in rep["findings"]}


def status(rep: dict, target: str) -> str:
    return rep["assessments"][target]["status"]


def doc(lang, **kw) -> TaggedDoc:
    d = TaggedDoc("lang-case", lang=lang, **kw)
    body(d)
    return d


# ---------------------------------------------------------------------------------------------------------------------
# values that are RFC 3066 Language-Tags: never 7.2-29, whatever PAC's check says about them
# ---------------------------------------------------------------------------------------------------------------------
WELL_FORMED_RFC3066 = [
    # value, the PAC rule expected (None: none), wcag-2.1-AA
    ("de-DE", None, PASS),
    ("en", None, PASS),
    ("EN-us", None, PASS),                      # tags are case insensitive (RFC 3066 2.1)
    ("gsw", None, PASS),                        # a 3-letter code
    ("zh-Hant-TW", None, PASS),
    ("de-DE-1996", None, PASS),
    ("Deutsch", PAC_OTHER, FAIL),               # the real-world documents
    ("English", PAC_OTHER, FAIL),               # PDF/UA Reference Suite 1.1, PDFUA-Ref-2-09_Scanned.pdf
    ("zz", PAC_OTHER, FAIL),
    ("zz-ZZ", PAC_OTHER, FAIL),
    ("qaa", PAC_OTHER, FAIL),
    ("portugue", PAC_OTHER, FAIL),              # veraPDF 7.2-t29-pass-c/e: 8 letters, the upper bound of the grammar
    ("portugue-pt", PAC_OTHER, FAIL),           # veraPDF 7.2-t29-pass-a
    ("p", PAC_SYNTAX, FAIL),                    # veraPDF 7.2-t29-pass-d/f: 1 letter, the lower bound
    ("p-pt", PAC_SYNTAX, FAIL),                 # veraPDF 7.2-t29-pass-b
    ("en-US-US", PAC_SYNTAX, FAIL),             # a registered language, subtags BCP 47 does not allow there
    ("x-corpus", PAC_X, FAIL),                  # private use, which RFC 3066 2.2 reserves "x" for
    ("i-klingon", PAC_I, PASS),                 # a warning only
]


@pytest.mark.parametrize("value,pac_rule,wcag", WELL_FORMED_RFC3066, ids=[c[0] for c in WELL_FORMED_RFC3066])
def test_rfc3066_language_tag_is_not_a_7_2_29_violation(tmp_path, value, pac_rule, wcag):
    rep = report(tmp_path, doc(value), "wf")
    found = by_rule(rep)
    assert GRAMMAR not in found and GRAMMAR_UA2 not in found, found.get(GRAMMAR)
    assert {r for r in found if r in LANGUAGE_RULES} == ({pac_rule} if pac_rule else set())
    # nothing of PDF/UA-1 fails: the document is the PDF/UA-1-clean construction but for the Lang value
    assert not [r for r in found if r.startswith("EQX-UA1-")], sorted(found)
    assert status(rep, "pdfua-1") == PASS
    assert status(rep, "wcag-2.1-AA") == wcag
    assert status(rep, "wcag-2.0-A") == wcag            # 3.1.1 is level A
    if pac_rule:
        f = found[pac_rule]
        assert f["occurrences"] == 1 and value in f["locations"][0]["detail"]
        refs = {s["standard"]: s for s in f["references"]["standards"]}
        assert refs["pdfua-1"]["obligation"] == "warning"
        assert refs["wcag"]["sc"] == "3.1.1"
        assert pac_rule in rep["assessments"]["pdfua-1"]["warning_rules"]
        assert pac_rule not in rep["assessments"]["pdfua-1"]["failed_rules"]
        if pac_rule != PAC_I:
            assert f["severity"] == "error" and refs["wcag"]["obligation"] == "normative"
            assert "matterhorn" not in refs["pdfua-1"], "no Matterhorn failure condition is claimed for PDF/UA-1"
            assert pac_rule in rep["assessments"]["wcag-2.1-AA"]["failed_rules"]
            # the default result basis (declared PDF/UA part + wcag-2.1-AA) still says "errors found"
            assert rep["summary"]["result"] == "errors_found"


# ---------------------------------------------------------------------------------------------------------------------
# values that are no Language-Tag: 7.2-29, through the grammar itself (not only through PAC's check)
# ---------------------------------------------------------------------------------------------------------------------
NOT_A_LANGUAGE_TAG = ["en_US", "en US", "en-", "-en", "e1", "abcdefghi", "en-abcdefghi", "de--DE", "en_US!", "deutsch!",
                      ""]


@pytest.mark.parametrize("value", NOT_A_LANGUAGE_TAG, ids=[v or "empty" for v in NOT_A_LANGUAGE_TAG])
def test_value_outside_the_grammar_is_a_7_2_29_violation(tmp_path, value):
    rep = report(tmp_path, doc(value), "bad")
    found = by_rule(rep)
    assert GRAMMAR in found and GRAMMAR_UA2 in found
    f = found[GRAMMAR]
    assert f["severity"] == "error" and f["non_pac_occurrences"] >= 1, "the grammar itself fails, not only PAC's check"
    assert any("is not a Language-Tag" in loc["detail"] for loc in f["locations"])
    assert status(rep, "pdfua-1") == FAIL and GRAMMAR in rep["assessments"]["pdfua-1"]["failed_rules"]
    assert status(rep, "pdfua-2") == FAIL and status(rep, "wcag-2.1-AA") == FAIL
    assert PAC_OTHER not in found and PAC_X not in found


def test_non_ascii_value_is_a_7_2_29_violation(tmp_path):
    d = doc("en-US")
    d.catalog_extra["Lang"] = Str(b"\xfe\xff" + "dé".encode("utf-16-be"))
    found = by_rule(report(tmp_path, d, "nonascii"))
    assert GRAMMAR in found and found[GRAMMAR]["non_pac_occurrences"] >= 1


# ---------------------------------------------------------------------------------------------------------------------
# the PDF encoding of the text string does not change the verdict (ISO 32000-1 7.9.2.2)
# ---------------------------------------------------------------------------------------------------------------------
@pytest.mark.parametrize("value,expected", [("de-DE", None), ("Deutsch", PAC_OTHER), ("en_US", GRAMMAR)],
                         ids=["de-DE", "Deutsch", "en_US"])
@pytest.mark.parametrize("encoding", ["pdfdoc", "utf16be", "hex"])
def test_text_string_encoding_does_not_matter(tmp_path, value, expected, encoding):
    d = doc("en-US")
    if encoding == "utf16be":
        d.catalog_extra["Lang"] = Str(b"\xfe\xff" + value.encode("utf-16-be"), hex=True)
    elif encoding == "hex":
        d.catalog_extra["Lang"] = Str(value.encode("latin-1"), hex=True)
    else:
        d.catalog_extra["Lang"] = Str(value.encode("latin-1"))
    found = by_rule(report(tmp_path, d, "enc"))
    assert {r for r in found if r in LANGUAGE_RULES} - {GRAMMAR_UA2} == ({expected} if expected else set())


# ---------------------------------------------------------------------------------------------------------------------
# Lang entries other than the catalog's: the grammar applies to each of them, PAC's check to the document language only
# ---------------------------------------------------------------------------------------------------------------------
def test_structure_element_lang_is_checked_against_the_grammar_only(tmp_path):
    d = TaggedDoc("lang-se", lang="de-DE")
    p = d.page()
    p.heading(1, "Sprache")
    p.para("Ein Absatz.").extra["Lang"] = "Deutsch"         # an RFC 3066 Language-Tag
    found = by_rule(report(tmp_path, d, "se-ok"))
    assert not {r for r in found if r in LANGUAGE_RULES}

    d = TaggedDoc("lang-se", lang="de-DE")
    p = d.page()
    p.heading(1, "Sprache")
    p.para("Ein Absatz.").extra["Lang"] = "de_DE"           # no Language-Tag
    found = by_rule(report(tmp_path, d, "se-bad"))
    assert GRAMMAR in found and found[GRAMMAR]["non_pac_occurrences"] == found[GRAMMAR]["occurrences"] == 1
    assert "structure element" in found[GRAMMAR]["locations"][0]["detail"]


# ---------------------------------------------------------------------------------------------------------------------
# no catalog Lang
# ---------------------------------------------------------------------------------------------------------------------
def _without_catalog_lang(element_lang: str | None, furniture: bool = False) -> TaggedDoc:
    d = TaggedDoc("lang-nocat", lang=None, outline=False)   # outline items take their language from the catalog
    if element_lang is not None:
        d.top.extra["Lang"] = element_lang                  # the Document element: every tagged text inherits it
    body(d, furniture)                                      # furniture: a page number in an Artifact, without a Lang

    def metadata_language(doc: TaggedDoc, ctx: dict) -> None:   # x-default needs a catalog Lang (7.2-33)
        doc.w.set(ctx["catalog"]["Metadata"], Stream({"Type": Name("Metadata"), "Subtype": Name("XML")},
                                                     xmp(doc.title, doc.ua_part, lang_alt="en-US")))
    d.hooks.append(metadata_language)
    return d


def test_missing_catalog_lang_with_language_on_every_text_meets_pdfua1(tmp_path):
    """ISO 32000-1 14.9.2.1: the document language "may be specified" in the catalog. With a Lang on the Document
    element the language of all text is determined; PDF/UA-1 is met, PAC's "document language" is not."""
    rep = report(tmp_path, _without_catalog_lang("en-US"), "nocat-ok")
    found = by_rule(rep)
    assert GRAMMAR not in found, "a missing Lang is not a malformed Lang value"
    assert not [r for r in found if r.startswith("EQX-UA1-")], sorted(found)
    assert status(rep, "pdfua-1") == PASS
    assert found[PAC_SYNTAX]["locations"][0]["detail"] == "document language missing"
    assert found[PAC_SYNTAX]["severity"] == "error"
    assert status(rep, "wcag-2.1-AA") == FAIL and PAC_SYNTAX in rep["assessments"]["wcag-2.1-AA"]["failed_rules"]
    assert "EQX-UA2-8.4.4-1" in found and status(rep, "pdfua-2") == FAIL      # PDF/UA-2 does require the catalog Lang


def test_missing_catalog_lang_with_text_of_undetermined_language_fails_pdfua1(tmp_path):
    """The same document with a page number in an Artifact: that text is page content without a language of its own
    (ISO 14289-1 7.2, Matterhorn 11-001), which only a catalog Lang would have given it."""
    rep = report(tmp_path, _without_catalog_lang("en-US", furniture=True), "nocat-artifact")
    found = by_rule(rep)
    assert found["EQX-UA1-7.2-34"]["occurrences"] == 1 and GRAMMAR not in found
    assert status(rep, "pdfua-1") == FAIL


def test_missing_language_everywhere_still_fails_pdfua1(tmp_path):
    rep = report(tmp_path, _without_catalog_lang(None), "nocat-bad")
    found = by_rule(rep)
    assert "EQX-UA1-7.2-34" in found and "EQX-UA1-7.2-22" in found      # page text, Alt of the Figure
    assert GRAMMAR not in found
    assert status(rep, "pdfua-1") == FAIL and status(rep, "wcag-2.1-AA") == FAIL
    assert PAC_SYNTAX in found


def test_empty_catalog_lang_is_both(tmp_path):
    """An empty Lang states "unknown" (ISO 32000-1 14.9.2.2): no Language-Tag for 7.2-29, no document language for PAC."""
    found = by_rule(report(tmp_path, doc(""), "empty"))
    assert found[GRAMMAR]["occurrences"] == found[GRAMMAR]["non_pac_occurrences"] == 1
    assert found[PAC_SYNTAX]["locations"][0]["detail"] == "document language missing"


# ---------------------------------------------------------------------------------------------------------------------
# PDF 2.0: PAC defines its checks up to PDF 1.7; the grammar applies as before
# ---------------------------------------------------------------------------------------------------------------------
def test_pdf20_has_the_grammar_and_no_pac_language_rules(tmp_path):
    pdf20 = dict(version="2.0", ua_part=2, ua_rev=2024, outline=False, pdf2_namespace=True)
    rep = report(tmp_path, doc("Deutsch", **pdf20), "pdf20-wf")
    found = by_rule(rep)
    assert not {r for r in found if r in LANGUAGE_RULES}
    assert {PAC_SYNTAX, PAC_OTHER, PAC_X} <= {x["rule"] for x in rep["rules_not_applicable"]}
    assert not [r for r in found if r.startswith("EQX-UA2-")], sorted(found)
    assert status(rep, "pdfua-2") == PASS

    rep = report(tmp_path, doc("en_US", **pdf20), "pdf20-bad")
    found = by_rule(rep)
    assert GRAMMAR_UA2 in found and GRAMMAR in found
    assert status(rep, "pdfua-2") == FAIL


# ---------------------------------------------------------------------------------------------------------------------
# the catalogue
# ---------------------------------------------------------------------------------------------------------------------
def test_catalogue_separates_the_pac_language_rules_from_pdfua1():
    out = run("--list-rules")
    rules = {line.split("\t")[0]: line.split("\t") for line in out.stdout.decode().splitlines()}
    for rid in (PAC_SYNTAX, PAC_OTHER, PAC_X):
        assert rules[rid][1] == "error", "still an error finding: nothing was downgraded to a warning"
        assert rules[rid][2] == "document"
    assert rules[GRAMMAR][1] == "error" and rules[PAC_I][1] == "warning"
