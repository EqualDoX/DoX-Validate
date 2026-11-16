#!/usr/bin/env python3
"""PAC 26.1 on veraPDF's PDF/UA-1 test suite: detection rate and false-positive rate per tested requirement.

    python3 -I bench/analyze_pac_ua1.py bench/results/pac-ua1-2026-11-16

Reads readouts.jsonl (one record per suite file: PAC's verdict, its report's checkpoint table, the failed and warned
PDF/UA conditions of its "Results in Detail" view; README.md there says how they were read) and writes tables.md
next to it. Standard library only.

Counting, as for the other validators of the comparison: a suite file tests one requirement, the veraPDF test named
by the file (`7.2-t03-fail-a.pdf` tests 7.2-3; `7-18.3-t01-pass-a.pdf` is 7.18.3-1). A fail file is detected when
PAC reports that requirement; a pass file is a false positive when PAC reports it there. Any other finding counts
for neither. A file PAC did not process reports nothing; it stays in the denominator. Only failed conditions count:
PAC's verdict ignores warnings, so a warning is not PAC asserting a violation (tables.md gives the figures with the
warnings as well).

PAC_MAP says which veraPDF tests each PAC condition checks. It was fixed before the suite results were tallied.
Bases:
  catalogue  the DoX-Validate catalogue (catalog/rules.json) lists the PAC condition as the counterpart of the
             veraPDF rule; re-read against both texts
  text       no catalogue entry; PAC's condition and the veraPDF test describe the same requirement
  nesting    PAC checks nesting per element type: 'Invalid use of a "X" structure element' is an X with a parent or
             kid that is not allowed. It counts for every nesting test that names X. This is the granularity of the
             Matterhorn failure conditions 09-004 (tables), 09-005 (lists) and 09-006 (TOC), through which cpdf
             was matched
"""
from __future__ import annotations

import collections
import json
import re
import sys
from pathlib import Path

TABLE_NEST = {f"7.2-{i}" for i in (3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 36, 37, 38, 39)}
LIST_NEST = {f"7.2-{i}" for i in (17, 18, 19, 20, 40)}
TOC_NEST = {f"7.2-{i}" for i in (26, 27, 28)}

PAC_MAP: dict[str, tuple[set[str], str]] = {
    # clause 5: one PAC check for the PDF/UA identification schema (presence, part value, pdfuaid prefixes)
    "CheckFailDocumentContainsPDFUAIdentification": ({f"5-{i}" for i in range(1, 6)}, "catalogue (5-1), text"),
    "CheckFailMarkedIsTrue": ({"6.2-1"}, "catalogue"),
    "CheckFailNoArtifactInTaggedContent": ({"7.1-1"}, "catalogue"),
    "CheckFailNoTaggedContentInArtifacts": ({"7.1-2"}, "catalogue"),
    "CheckFailContentIsTaggedOrArtifactedText": ({"7.1-3"}, "catalogue"),
    "CheckFailContentIsTaggedOrArtifactedPath": ({"7.1-3"}, "catalogue"),
    "CheckFailContentIsTaggedOrArtifactedImage": ({"7.1-3"}, "catalogue"),
    "CheckFailSuspectsIsFalse": ({"7.1-4"}, "catalogue"),
    "CheckFailNonStandardStructureTypeIsRemapped": ({"7.1-5"}, "catalogue"),
    "CheckFailNoCircularMappings": ({"7.1-6"}, "catalogue"),
    "CheckFailStandardStructureTypeIsNotRemapped": ({"7.1-7"}, "catalogue"),
    "CheckFailMetadataInCatalogExists": ({"7.1-8"}, "catalogue"),
    "CheckFailDocumentContainsTitle": ({"7.1-9"}, "catalogue"),
    "CheckFailDisplayDocTitleIsTrue": ({"7.1-10"}, "catalogue"),
    "CheckFailStructureElementHasParentKey": ({"7.1-12"}, "catalogue"),
    "CheckFailOutlineItemHasLanguage": ({"7.2-2"}, "catalogue"),
    "CheckFailActualTextHasLanguage": ({"7.2-21", "7.2-30"}, "text"),
    "CheckFailAltTextHasLanguage": ({"7.2-22", "7.2-31"}, "text"),
    "CheckFailExpansionTexHasLanguage": ({"7.2-23", "7.2-32"}, "text"),
    "CheckFailContentsEntryHasLanguage": ({"7.2-24"}, "catalogue"),
    "CheckFailTUEntryHasLanguage": ({"7.2-25"}, "catalogue"),
    "CheckFailValidLanguageSyntax": ({"7.2-29"}, "catalogue"),
    "CheckFailValidLanguagePrimaryTagX": ({"7.2-29"}, "catalogue"),
    "CheckFailValidLanguagePrimaryTagOther": ({"7.2-29"}, "catalogue"),
    "CheckFailTextContentHasLanguage": ({"7.2-34"}, "catalogue"),
    "CheckFailFigureWithNoAltText": ({"7.3-1"}, "catalogue"),
    "CheckFailFirstHeadingIsH1": ({"7.4.2-1"}, "text"),
    "CheckFailNoHeadingsSkipped": ({"7.4.2-1"}, "text"),
    "CheckFailOnlyOneHPerNode": ({"7.4.4-1"}, "catalogue"),
    "CheckFailCaptionHeadingStructureTypesNotMixed": ({"7.4.4-2", "7.4.4-3"}, "text"),
    "CheckFailFormulaWithNoAltText": ({"7.7-1"}, "catalogue"),
    "CheckFailNoteTagHasID": ({"7.9-1"}, "catalogue"),
    "CheckFailNoteTagHasUniqueID": ({"7.9-2"}, "catalogue"),
    "CheckFailOCConfDictHasName": ({"7.10-1"}, "catalogue"),
    "CheckFailOCConfDictHasNoAS": ({"7.10-2"}, "catalogue"),
    "CheckFailFSDictsContainsFAndUF": ({"7.11-1"}, "catalogue"),
    "CheckFailXFAIsNotDynamic": ({"7.15-1"}, "catalogue"),
    "CheckFailAccessibleEncyptionSettings": ({"7.16-1"}, "catalogue"),
    "CheckFailAnnotationsInAnnotTag": ({"7.18.1-1"}, "catalogue"),
    "CheckFailAnnotationsInAnnotTag2": ({"7.18.1-1"}, "catalogue"),
    "CheckFailAnnotationHasAltText": ({"7.18.1-2", "7.18.5-2"}, "text"),
    "CheckFailFormFieldHasTUKey": ({"7.18.1-3"}, "catalogue"),
    "CheckFailDocumentHasNoTrapNetAnnots": ({"7.18.2-1"}, "catalogue"),
    "CheckFailPagesWithAnnotsHaveTabOrderS": ({"7.18.3-1"}, "catalogue"),
    "CheckFailWidgetAnnotationsInFormTag": ({"7.18.4-1"}, "catalogue"),
    "CheckFailWidgetAnnotationsInFormTag2": ({"7.18.4-1"}, "catalogue"),
    "CheckFailLinkAnnotationsInLinkTag": ({"7.18.5-1"}, "catalogue"),
    "CheckFailLinkAnnotationsInLinkTag2": ({"7.18.5-1"}, "catalogue"),
    "CheckFailPrinterMarkAnnotationaNotTagged": ({"7.18.8-1"}, "catalogue"),
    "CheckFailDocuementContainsNoReferenceXObjects": ({"7.20-1"}, "catalogue"),
    "CheckFailFontType0RegistryIsIdentical": ({"7.21.3.1-1"}, "catalogue"),
    "CheckFailFontType0RegistryIsOrdering": ({"7.21.3.1-1"}, "catalogue"),
    "CheckFailFontType0SupplementIsNotLess": ({"7.21.3.1-1"}, "catalogue"),
    "CheckFailFontType2HasCIDToGIDMap": ({"7.21.3.2-1"}, "catalogue"),
    "CheckFailCMapPredefinedOrEmbedded": ({"7.21.3.3-1"}, "catalogue"),
    "CheckFailWModeInDictAndStreamIdentical": ({"7.21.3.3-2"}, "catalogue"),
    "CheckFailCMapOnlyReferencesToPredefinedCMaps": ({"7.21.3.3-3"}, "catalogue"),
    "CheckFailFontsAreEmbedded": ({"7.21.4.1-1"}, "catalogue"),
    "CheckFailNonSymbolicTTFontContainsEncoding": ({"7.21.6-2"}, "catalogue"),
    "CheckFailNonSymbolicTTFontContainsOnlyAdobeGlyphNames": ({"7.21.6-2"}, "catalogue"),
    "CheckFailSymbolicTTFontContainsNoEncoding": ({"7.21.6-3"}, "catalogue"),
    "CheckFailCharactersUnicodeMappable": ({"7.21.7-1"}, "catalogue"),
}
for _t in ("Table", "TR", "TH", "TD", "THead", "TBody", "TFoot"):
    PAC_MAP[f"CheckFail{_t}TagInvalidSyntax"] = (TABLE_NEST, "nesting")
for _t in ("L", "LI", "Lbl", "LBody"):
    PAC_MAP[f"CheckFail{_t}TagInvalidSyntax"] = (LIST_NEST, "nesting")
for _t in ("TOC", "TOCI"):
    PAC_MAP[f"CheckFail{_t}TagInvalidSyntax"] = (TOC_NEST, "nesting")
PAC_MAP["CheckFailCaptionTagInvalidSyntax"] = (TABLE_NEST | LIST_NEST | TOC_NEST, "nesting")  # all three name Caption

# PAC's language-metadata check reads only the document catalog's /Lang. Without that entry it reports the same
# condition, once, for the missing document language: on every suite file without a catalog /Lang, also where all
# Lang values are valid (7.2-t21-pass-b). Found while verifying the matches: these conditions count for 7.2-29 only
# when the catalog has a /Lang entry, so that the finding is about a Lang value the test checks.
CATALOG_LANG_ONLY = {"CheckFailValidLanguageSyntax", "CheckFailValidLanguagePrimaryTagX",
                     "CheckFailValidLanguagePrimaryTagOther", "CheckWarnValidLanguagePrimaryTagI"}

# Fail files on which PAC reports the nesting violation on the offending element, of a type the test does not name
# (checked in the file's structure tree). Not counted (the rule above was fixed first); shown in the related figure.
OFFENDING_ELEMENT = {
    "7.2-t10-fail-a.pdf": ("CheckFailSpanTagInvalidSyntax", "a Span inside a TR"),
    "7.2-t19-fail-b.pdf": ("CheckFailSpanTagInvalidSyntax", "a Span inside an L"),
    "7.2-t27-fail-c.pdf": ("CheckFailPTagInvalidSyntax", "a P inside a TOC"),
}

# Warnings of the same requirements (only for the figures that include warnings)
WARN_MAP: dict[str, set[str]] = {
    "CheckWarnTablesAreRegular": {"7.2-15", "7.2-41", "7.2-42", "7.2-43"},
    "CheckWarnTBodyTagInvalidSyntax": TABLE_NEST,
    "CheckWarnValidLanguagePrimaryTagI": {"7.2-29"},
    "CheckWarnContentsEntryOnAnnotationIsWhiteSpace": {"7.18.1-2", "7.18.5-2"},
}

# Suite tests for which no PAC condition checks the same requirement. AMBIGUOUS lists the PAC conditions that are
# related but not the same requirement: never counted, but tables.md shows what counting them would change.
NO_COUNTERPART = {
    "7.1-11": "structure tree rooted in StructTreeRoot: no PAC check of its own (ambiguous: 'Structural parent tree "
              "not found' and untagged content are consequences, not this requirement)",
    "7.2-15": "intersecting table cells: PAC's only table-geometry check ('Irregular table row') is a warning",
    "7.2-33": "natural language of the metadata determinable (ambiguous: PAC's language-metadata conditions check "
              "validity, not that a language can be determined)",
    "7.2-41": "column and row counts: 'Irregular table row' is a warning",
    "7.2-42": "column and row counts: 'Irregular table row' is a warning",
    "7.2-43": "column and row counts: 'Irregular table row' is a warning",
    "7.5-1": "TH without Scope where Headers/IDs do not give the structure (ambiguous: PAC checks whether a header "
             "cell has associated cells, a different criterion)",
    "7.5-2": "as 7.5-1",
    "7.18.6.2-1": "media clip CT key: no PAC condition",
    "7.18.6.2-2": "media clip Alt key: no PAC condition",
    "7.18.7-1": "veraPDF's PDF/UA-1 profile has no rule 7.18.7-1 and reports these files as 7.11-1; the comparison "
                "counts them as missed for every validator, PAC included",
    "7.20-2": "Form XObject content in structure elements: no PAC condition",
    "7.21.4.2-1": "Type 1 CharSet completeness: no PAC condition",
    "7.21.4.2-2": "CIDSet completeness: no PAC condition",
    "7.21.5-1": "glyph widths: no PAC condition",
    "7.21.7-2": "ToUnicode values 0, U+FEFF, U+FFFE (ambiguous: PAC checks that a Unicode mapping exists; Matterhorn "
                "31-027 rather than 31-028/31-029)",
    "7.21.8-1": ".notdef glyph references: no PAC condition",
}
AMBIGUOUS: dict[str, set[str]] = {
    "7.1-11": {"CheckFailStructuralParentTreeMissing", "CheckFailContentIsTaggedOrArtifactedText",
               "CheckFailContentIsTaggedOrArtifactedPath", "CheckFailContentIsTaggedOrArtifactedImage"},
    "7.2-33": {"CheckFailValidLanguageSyntax", "CheckFailValidLanguagePrimaryTagX",
               "CheckFailValidLanguagePrimaryTagOther"},
    "7.5-1": {"THHasAssociatedCells_CheckTHs_TableHeaderCellHasNoAssociatedSubcells"},
    "7.5-2": {"THHasAssociatedCells_CheckTHs_TableHeaderCellHasNoAssociatedSubcells"},
    "7.21.7-2": {"CheckFailCharactersUnicodeMappable"},
}


def short(c: str) -> str:
    return c.replace("CheckFail", "F:").replace("CheckWarn", "W:")


def key(t: str) -> list[int]:
    return [int(x) for x in re.split(r"[.-]", t)]


def pct(a: int, b: int) -> str:
    return f"{100 * a / b:.1f} %"


def main(res: Path) -> None:
    recs = [json.loads(line) for line in open(res / "readouts.jsonl", encoding="utf-8")]
    rows = []
    for r in recs:
        fails = {f["condition"] for f in r.get("findings", []) if f["status"] == "fail"}
        warns = {f["condition"] for f in r.get("findings", []) if f["status"] == "warn"}

        def counted(c: str) -> bool:
            return c not in CATALOG_LANG_ONLY or r.get("catalog_lang") is not None

        ids = set().union(*(PAC_MAP.get(c, (set(), ""))[0] for c in fails if counted(c)))
        wids = ids | set().union(*(WARN_MAP.get(c, set()) for c in warns if counted(c)))
        related = bool(AMBIGUOUS.get(r["test"], set()) & (fails | warns)) or (
            r["file"] in OFFENDING_ELEMENT and OFFENDING_ELEMENT[r["file"]][0] in fails)
        lang_only = r["test"] == "7.2-29" and not r["test"] in ids and bool(fails & CATALOG_LANG_ONLY)
        rows.append({**r, "fails": fails, "warns": warns, "hit": r["test"] in ids, "hit_w": r["test"] in wids,
                     "hit_rel": r["test"] in ids or related, "related": related, "lang_only": lang_only,
                     "rejected": (r.get("verdict") or "").startswith("This PDF file is not")})
    nf = sum(1 for r in rows if r["expected"] == "fail")
    npass = sum(1 for r in rows if r["expected"] == "pass")
    status = collections.Counter(r["status"] for r in rows)
    unmapped = sorted({c for r in rows for c in r["fails"] if c not in PAC_MAP})
    out = ["# PAC 26.1 on veraPDF's PDF/UA-1 test suite", "",
           f"Generated by `bench/analyze_pac_ua1.py` from `readouts.jsonl` ({len(rows)} files: {nf} fail, {npass} pass).", "",
           "## Processing", "",
           "| status | files |", "|---|---|"] + [f"| {k} | {v} |" for k, v in sorted(status.items())]
    out += ["", "Files without a PAC result (they report nothing and stay in the denominator):", ""]
    out += [f"- `{r['file']}` ({r['status']}): {r.get('note', '')}" for r in rows if r["status"] != "read"]
    probs = [r for r in rows if r.get("problems") or r.get("unresolved")]
    out += ["", f"Read-outs with a consistency problem (report vs detail view) or an unresolved row: {len(probs)}"]
    for r in probs:
        out.append(f"- `{r['file']}`: {'; '.join(r.get('problems') or [])} "
                   f"{[u.get('label') for u in r.get('unresolved') or []]}")

    def table(title, field):
        tp = sum(1 for r in rows if r["expected"] == "fail" and r[field])
        fp = sum(1 for r in rows if r["expected"] == "pass" and r[field])
        return f"| {title} | {tp} / {nf} | {pct(tp, nf)} | {fp} / {npass} | {pct(fp, npass)} |"

    out += ["", "## Test level (the tested requirement)", "",
            "| counting | fail files detected | detection rate | pass files flagged | false-positive rate |",
            "|---|---|---|---|---|",
            table("**failed PAC conditions (chart)**", "hit"),
            table("failed and warned conditions", "hit_w"),
            table("failed conditions, related matches counted as well", "hit_rel"), "",
            "Related matches: the AMBIGUOUS conditions of a test without a PAC counterpart, and the nesting findings on "
            "the offending element of a type the test does not name (OFFENDING_ELEMENT). Neither is counted in the "
            "first row.", "",
            "## Verdict level (PAC rejects the whole file)", "",
            "| | fail files rejected | pass files rejected |", "|---|---|---|",
            f"| PAC verdict 'not PDF/UA compliant' | {sum(1 for r in rows if r['expected'] == 'fail' and r['rejected'])} / {nf}"
            f" | {sum(1 for r in rows if r['expected'] == 'pass' and r['rejected'])} / {npass} |"]
    if unmapped:
        out += ["", "Failed PAC conditions that map to no veraPDF PDF/UA-1 test: " + ", ".join(f"`{short(c)}`" for c in unmapped)]
    by_test = collections.defaultdict(list)
    for r in rows:
        by_test[r["test"]].append(r)
    out += ["", "## Per tested requirement", "",
            "| test | fail files detected | pass files flagged | PAC conditions that count | note |", "|---|---|---|---|---|"]
    for t in sorted(by_test, key=key):
        rs = by_test[t]
        f = [r for r in rs if r["expected"] == "fail"]
        p = [r for r in rs if r["expected"] == "pass"]
        conds = sorted({short(c) for c, (ts, _) in PAC_MAP.items() if t in ts})
        if len(conds) > 3:
            conds = [f"{len(conds)} nesting conditions"]
        note = NO_COUNTERPART.get(t, "") or (
            "counted only where the catalog has a /Lang entry (CATALOG_LANG_ONLY)" if t == "7.2-29" else "")
        out.append(f"| {t} | {sum(r['hit'] for r in f)} / {len(f)} | {sum(r['hit'] for r in p)} / {len(p)} | "
                   f"{', '.join(conds) or '–'} | {note} |")
    out += ["", "## Fail files not detected", "",
            "What PAC reported instead (failed `F:`, warned `W:` conditions of its PDF/UA tab):", ""]
    for r in rows:
        if r["expected"] == "fail" and not r["hit"]:
            got = sorted(short(c) for c in r["fails"]) + sorted(short(c) for c in r["warns"])
            why = ""
            if r["file"] in OFFENDING_ELEMENT:
                why = f" (flags the offending element, {OFFENDING_ELEMENT[r['file']][1]}; outside the rule, not counted)"
            elif r["related"]:
                why = " (ambiguous match, not counted)"
            elif r["lang_only"]:
                why = " (no catalog /Lang: PAC's language finding is the missing document language, not the tested value)"
            out.append(f"- `{r['file']}` ({r['test']}, {r['status']}): {', '.join(got) or 'nothing'}{why}")
    out += ["", "## Pass files with failed conditions", "",
            "Counted as a false positive only when the condition maps to the file's tested requirement.", ""]
    for r in rows:
        if r["expected"] == "pass" and r["fails"]:
            out.append(f"- `{r['file']}` ({r['test']}): {', '.join(sorted(short(c) for c in r['fails']))}"
                       f"{' **false positive**' if r['hit'] else ''}")
    (res / "tables.md").write_text("\n".join(out) + "\n", encoding="utf-8")
    print("\n".join(out[:40]))


if __name__ == "__main__":
    main(Path(sys.argv[1]))
