# PAC 2026.1 on veraPDF's PDF/UA-1 test suite

Detection quality of PAC, the PDF Accessibility Checker, on the same 297 test files as the other PDF/UA validators of
the comparison: veraPDF's PDF/UA-1 test suite, 156 fail files and 141 pass files. The run took place on 2026-11-16.
Speed and memory were not measured; [docs/BENCHMARKS.md](../../../docs/BENCHMARKS.md#pac-pdf-accessibility-checker)
gives the reasons.

| | fail files detected | pass files flagged |
|---|---|---|
| PAC 26.1.0.0 | **98 / 156 (62.8 %)** | **11 / 141 (7.8 %)** |

- `readouts.jsonl`: one record per suite file. It holds PAC's verdict, the checkpoint table of PAC's exported PDF/UA
  report, and the failed and warned rows of PAC's PDF/UA results, as read and resolved to PAC's condition keys.
- `tables.md`: every count, per tested requirement, and every file PAC missed or flagged with what it reported.
- [`bench/analyze_pac_ua1.py`](../../analyze_pac_ua1.py): the matching rules, and the script that writes
  `tables.md`. Run `python3 -I bench/analyze_pac_ua1.py bench/results/pac-ua1-2026-11-16`; it needs the standard
  library only.

## Setup

| | |
|---|---|
| PAC | 26.1.0.0 (PAC 2026.1), official installer, PDF/UA tab (standard PDF/UA-1, Matterhorn Protocol) |
| Platform | Wine 11.20 Staging with .NET Framework 4.8 in a dedicated prefix, on Arch Linux (kernel 7.3.4), Intel Core i9-14900KF; PAC pinned to CPUs 0-7, on a hidden Hyprland output. axes4 ships PAC for Windows only. |
| Corpus | `tests/corpus/public/manifest.json`, group `verapdf-ua1`: 297 files, each checked against the SHA-256 pinned there before the run |
| Run | 2026-11-16, 06:20 to 12:40 with the repeat runs, one file at a time, 72 s per file on average including the read-out |

## How PAC's results were read

PAC has no command-line interface or machine-readable output, so it was driven through its own window with EqualDoX's
internal PAC tooling (not part of this repository). Nothing in PAC was modified or reverse-engineered. For each file:

1. **Open the file.** It was opened through PAC's "Open document" dialog; PAC checks a file as soon as it opens.
2. **Export PAC's PDF/UA report.** The report gives the verdict and the passed, warned and failed count of every
   checkpoint. A read-out counts only when the exported report names the file.
3. **Read the detailed results.** Where the report counts warned or failed checks, PAC's "Results in Detail" view
   was expanded and read by OCR.
4. **Resolve the conditions.** Each failed or warned row was resolved to its PAC condition key: the keys PAC uses
   for its messages, the same keys as the PAC sources in [`catalog/rules.json`](../../../catalog/rules.json).
5. **Check against the report.** Every checkpoint with a failed (warned) count in the report must have a resolved
   failed (warned) condition below it. All 293 read-outs pass this check, and no row was left unresolved.

Only the PDF/UA tab counts. PAC's verdict is based on it, and it is PAC's PDF/UA-1 check. The WCAG and Quality tabs
were not used.

### Files without a PAC result

All four are fail files. They count as reporting nothing and stay in the denominators.

| file | what PAC did |
|---|---|
| `7.21.3.3-t01-fail-a.pdf` | Error dialog "One or more checks could not be completed" with the message "Invalid predefined CMap name."; no result, the same on a second run. |
| `7.5-t01-fail-a.pdf`, `7.5-t02-fail-a.pdf` | The same error dialog with the message "Unkown symbol: ..." (sic); no result, the same on a second run. |
| `7.1-t05-fail-d.pdf` | The check does not end. Two runs produced no report within 288 s each. A third run was watched for 480 s: PAC still showed "Check is running ..." with three checkpoints unfinished (Structure Elements, Structure tree, Role mapping), and PAC.exe kept using about two cores. The file has one page, four structure elements and a circular role map. |

## Counting

The same rules apply as for the other validators of the comparison:

- **Tested requirement.** A suite file tests one requirement: the veraPDF test named by the file. For example,
  `7.2-t03-fail-a.pdf` tests 7.2-3, and `7-18.3-t01-pass-a.pdf` tests 7.18.3-1.
- **Detection.** A fail file is detected when PAC reports that requirement.
- **False positive.** A pass file is a false positive when PAC reports that requirement on it.
- **Other findings.** Any other finding counts for neither.

Only failed conditions count. PAC's own verdict ignores warnings, so a warning is not PAC asserting a violation.
`tables.md` also gives the figures with the warnings included: 105 / 156 detected, 11 / 141 flagged.

### Which PAC condition reports which requirement

`PAC_MAP` in `bench/analyze_pac_ua1.py` assigns each PAC condition the veraPDF tests that check the same requirement.
It was fixed before the results were tallied. Each assignment has one of three bases:

- **Catalogue.** DoX-Validate's catalogue lists the PAC condition as the counterpart of the veraPDF rule. These pairs
  were re-read against both texts.
- **Text.** PAC's condition and the veraPDF test describe the same requirement, for example the language of ActualText,
  Alt and E, or headings.
- **Nesting.** PAC checks structure nesting per element type. "Invalid use of a "TR" structure element" means a TR with
  a parent or kid that is not allowed. Such a finding counts for every table, list or TOC nesting test that names
  that element type. This is the granularity of the Matterhorn failure conditions 09-004, 09-005 and 09-006, through
  which cpdf was matched.

  Because of it, PAC is credited for six fail files on which veraPDF reports a neighbouring rule: 7.2-39 or 7.2-16
  for 7.2-t03-fail-b/c, 7.2-40 for 7.2-t19-fail-a/c, and 7.2-28 for 7.2-t27-fail-a/b. On those six files PAC flags
  the Caption elements in question.

One rule was tightened while the matches were verified. It lowers PAC's count:

- **Catalog Lang only.** PAC's language-metadata check ("Document language metadata contains a syntax error") reads
  only the document catalog's /Lang. When the catalog has no /Lang, PAC reports the same condition once, for the
  missing document language. That holds on all 39 suite files without a catalog /Lang, including files whose Lang
  values are all valid (`7.2-t21-pass-b.pdf`).
- **What counts now.** The condition counts for 7.2-29 ("a Lang value shall be a language identifier") only where the
  catalog has a /Lang entry.
- **Effect.** Ten 7.2-29 fail files carry their invalid value on a structure element or in marked content, and have
  no catalog /Lang. On them PAC reports the missing document language, not the tested value, so they count as
  missed. Without this rule PAC would have 108 / 156.

### Not counted: ambiguous and related findings

These are listed in `tables.md`, and its third figure counts them as well: 104 / 156 detected, 14 / 141 flagged.

**Tests without a PAC counterpart.** For these veraPDF tests no PAC condition checks the same requirement, so PAC
cannot be credited:

- 7.18.6.2-1 and 7.18.6.2-2: the media clip CT and Alt keys.
- 7.20-2: Form XObject content in structure elements.
- 7.21.4.2-1 and 7.21.4.2-2: CharSet and CIDSet completeness.
- 7.21.5-1: glyph widths.
- 7.21.8-1: .notdef references.
- 7.2-15 and 7.2-41 to 7.2-43: table geometry. PAC's "Irregular table row" exists, but only as a warning.

**Ambiguous: related PAC conditions, a different requirement.** These are not counted:

- 7.1-11, a structure tree rooted in StructTreeRoot. PAC has no check of its own; "Structural parent tree not found"
  and untagged content are consequences, not this requirement.
- 7.2-33, a natural language for the metadata that can be determined. PAC's language-metadata conditions check
  validity, not whether a language can be determined.
- 7.5-1 and 7.5-2, a TH without Scope. PAC checks whether a header cell has associated cells instead, a different
  criterion. PAC reported it on two 7.5-1 fail files and two 7.5-1 pass files.
- 7.21.7-2, invalid ToUnicode values. PAC checks that a Unicode mapping exists at all.

**The offending element, of a type the test does not name.** PAC flags the element that breaks the nesting rule, but
the nesting rule above was fixed first, so these are not counted. Each was checked in the file's structure tree:

- `7.2-t10-fail-a.pdf`: a Span inside a TR (7.2-10).
- `7.2-t19-fail-b.pdf`: a Span inside an L (7.2-19).
- `7.2-t27-fail-c.pdf`: a P inside a TOC (7.2-27).

**7.18.7-1.** veraPDF's PDF/UA-1 profile has no rule of this number; veraPDF, DoX-Validate and VellumPdf report the
four fail files as 7.11-1. The comparison counts them as missed for every validator, so PAC's
"F or UF key missing in file specification" on two of them is not counted either.

## False positives

The 11 pass files PAC flags for their own tested requirement:

| test | files | PAC condition | note |
|---|---|---|---|
| 7.2-29 | `7.2-t29-pass-a` to `-f` | Document language metadata contains a syntax error | Catalog Lang values `portugue-pt`, `p-pt`, `portugue`, `p`. They are valid under RFC 3066, the syntax ISO 32000-1 14.9.2 refers to; PAC applies a stricter one. |
| 7.18.1-3 | `7.18.1-t03-pass-b` to `-d` | Alternate field name entry missing in form field | Fields without TU: one whose Form element carries Alt (b), one hidden widget (c), one widget outside the page (d). veraPDF accepts all three. |
| 7.21.3.1-1 | `7.21.3.1-t01-pass-d` | CMap "Supplement" entries are incompatible | |
| 7.21.7-1 | `7.21.7-t01-pass-a` | Characters in a text object cannot be mapped to Unicode | |

## Verdict level

PAC's verdict "This PDF file is not PDF/UA compliant" appears on 121 of 156 fail files and on 31 of 141 pass files.
A rejection counts at verdict level whatever the reason, so the verdict figures are not comparable with the
test-level figures above.
