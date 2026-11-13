# Standards coverage

This document describes what `dox-validate` checks, how it turns rule results into assessments, and what it
leaves for a person to evaluate. All counts below come from the catalogue files in [`catalog/`](../catalog) and can
be reproduced with:

```console
$ python3 -I tools/catalog_stats.py
```

The script uses only the Python standard library. It computes target membership with the same functions the
build uses ([`tools/gen_catalog.py`](../tools/gen_catalog.py)) to embed the catalogue, so its numbers are the ones
the binary works with.

> **Automated validation cannot prove conformance.** A report without failures means that the machine-checkable
> rules found no violation. It does not show that a document conforms to PDF/UA, WTPDF, WCAG or EN 301 549, and it
> does not show compliance with any law or procurement requirement. Many requirements of these standards can
> only be evaluated by a person, and every report lists them (`manual_required`).

## Sources of the rules

The catalogue (`catalog/rules.json`, 2008 rules) is the single source of truth for rule ids, titles,
severities and standards mappings. It is compiled into the binary at build time. The rules reproduce the
automated checks of two widely used reference validators. Neither tool is run by `dox-validate` or needed to run it.

| Rule ids | Reproduce | Standard |
|---|---|---|
| `EQX-UA1-*` | veraPDF 1.30.2, PDF/UA-1 profile (`ua1`) | ISO 14289-1 (PDF/UA-1) |
| `EQX-UA2-*` | veraPDF 1.30.2, PDF/UA-2 profile (`ua2`) | ISO 14289-2 (PDF/UA-2) |
| `EQX-TS32005-*` | veraPDF 1.30.2, structure containment rules of the `ua2`, `wt1a` and `wt1r` profiles | ISO/TS 32005 (PDF 1.7 and 2.0 structure namespace inclusion), applied through PDF/UA-2 and WTPDF |
| `EQX-WTPDF-*` | veraPDF 1.30.2, WTPDF profiles (`wt1a`, `wt1r`) | Well-Tagged PDF 1.0, accessibility and reuse levels |
| `EQX-PAC-*` | PAC 2026 (26.1.0.0), "PDF/UA" and "WCAG" tabs | PDF/UA-1 (via the Matterhorn Protocol) and WCAG 2.1 |
| `EQX-PACQ-*` | PAC 2026 (26.1.0.0), "Quality" tab | none (recommendations) |
| `EQX-WCAG-*` | EqualDoX's own colour checks | WCAG 1.4.1, 1.4.3, 1.4.11 |

How the rules relate to the reference checks:

- **One rule per requirement.** When a veraPDF rule and a PAC condition test the same requirement, they are merged
  into one rule id. The merged rule reports a violation if either definition is violated (union semantics). A
  location found only through the PAC definition has a `detail` starting with `PAC:`. A rule's severity is the
  highest severity of the reference checks it stands for.
- **veraPDF semantics.** veraPDF evaluates PDF/UA-1 rules with PDF 1.7 structure semantics, and PDF/UA-2 and WTPDF
  rules with ISO 32000-2 semantics. `dox-validate` does the same.
- **Every use is checked.** By default, `dox-validate` checks every painting of a content stream that is used more
  than once, such as a Form XObject drawn on several pages or in different tagging contexts. veraPDF checks such a
  stream only at its first use. `--reference-dedup` switches to veraPDF's behaviour, which is useful for
  differential comparisons with veraPDF.
- **PAC rules and PDF 2.0.** PAC 2026 defines its checks for PDF up to 1.7. On a PDF 2.0 document, the PAC-only
  rules (`EQX-PAC-*`, `EQX-PACQ-*`) are reported as not applicable (`rules_not_applicable`) and fail nothing.
  Merged rules then use only their veraPDF definition.
- **PAC is closed source.** Its conditions were reproduced from its observable behaviour, that is, its results
  and reports on test documents. Conditions that never triggered on a test document remain inferred.

### EqualDoX colour checks (`EQX-WCAG-*`)

| Rule | Severity | Fails a target | What it does |
|---|---|---|---|
| `EQX-WCAG-1.4.3-TextContrast` | error | yes (SC 1.4.3, AA and AAA targets) | Reports text with a contrast below 4.5:1 (3:1 for large text: 18 pt, or 14 pt bold), but only when the measurement is exact. Text and background must both be in DeviceGray or DeviceRGB. The background must be known without rendering: the white page, or one opaque rectangle painted before the text. The text must use normal paint, be inside its clip, and be neither an artifact nor inside a Figure, and its size class must be certain |
| `EQX-WCAG-1.4.3-TextContrastReview` | warning | no | The same measurement where a person has to decide: backgrounds from images, gradients or overlapping paint, CMYK or spot colours, soft masks and blend modes, partial clipping, outlined text, text inside a Figure, or an uncertain large-text status |
| `EQX-WCAG-1.4.11-NonTextContrastReview` | warning | no | Figure graphics and form-field borders or backgrounds that may be below 3:1 against a flat background |
| `EQX-WCAG-1.4.1-UseOfColorReview` | warning | no | A phrase on a line that differs from the neighbouring text only by colour: same font and size, less than 3:1 between the two colours. It is not reported when either phrase is underlined or framed as a link. A phrase counts as underlined when thin paths at its baseline together cover most of it, so a run of several links with one underline each counts too |

Uncertain cases are only ever warnings, so they cannot make a target fail.

## The catalogue in numbers

| Prefix | Rules | severity error | severity warning | kind violation | kind warning | kind recommendation |
|---|---:|---:|---:|---:|---:|---:|
| `EQX-UA1-*` | 106 | 106 | 0 | 106 | 0 | 0 |
| `EQX-UA2-*` | 91 | 91 | 0 | 91 | 0 | 0 |
| `EQX-TS32005-*` | 1636 | 1636 | 0 | 1636 | 0 | 0 |
| `EQX-WTPDF-*` | 2 | 2 | 0 | 2 | 0 | 0 |
| `EQX-PAC-*` | 146 | 77 | 69 | 77 | 69 | 0 |
| `EQX-PACQ-*` | 23 | 6 | 17 | 0 | 0 | 23 |
| `EQX-WCAG-*` | 4 | 1 | 3 | 1 | 3 | 0 |
| **total** | 2008 | 1919 | 89 | 1913 | 72 | 23 |

All 2008 rules are classed as automatically checkable, so all of them are *required*. If a required rule cannot
be evaluated on a document, that document cannot pass.

Rules per assessment target. A rule belongs to a target through a normative mapping in the catalogue. Rules
mapped to a target only as a warning are listed in the target's `warning_rules` when they fire, but they never
fail it.

| Target | rules with a normative mapping | of these error severity (can fail the target) | required rules | non-normative (listed as warning_rules only) |
|---|---:|---:|---:|---:|
| `pdfua-1` | 182 | 182 | 182 | 69 |
| `pdfua-2` | 1727 | 1727 | 1727 | 0 |
| `wtpdf-1.0-accessibility` | 1723 | 1723 | 1723 | 0 |
| `wtpdf-1.0-reuse` | 1710 | 1710 | 1710 | 0 |
| `wcag-2.0-A` | 105 | 105 | 105 | 69 |
| `wcag-2.0-AA` | 114 | 114 | 114 | 70 |
| `wcag-2.0-AAA` | 114 | 114 | 114 | 70 |
| `wcag-2.1-A` | 105 | 105 | 105 | 69 |
| `wcag-2.1-AA` | 114 | 114 | 114 | 71 |
| `wcag-2.1-AAA` | 114 | 114 | 114 | 71 |
| `wcag-2.2-A` | 34 | 34 | 34 | 4 |
| `wcag-2.2-AA` | 43 | 43 | 43 | 6 |
| `wcag-2.2-AAA` | 43 | 43 | 43 | 6 |

The large drop from WCAG 2.1 to 2.2 has one cause: SC 4.1.1 Parsing, which was removed in WCAG 2.2. 71 of the
114 rules with a normative WCAG mapping are mapped to 4.1.1 alone.

Six error-severity rules have no normative mapping to any target: `EQX-PACQ-FailLIContainsExactlyOneBody`,
`EQX-PACQ-FailMissingLinkTags`, `EQX-PACQ-FailOutlineIsNotPresentDespiteHeadings`,
`EQX-PACQ-FailOutlineIsPresentWithoutHeadings`, `EQX-PACQ-FailTOCILinkedMultiple` and
`EQX-PACQ-FailTOCIWithLinkedDescendants`. These are PAC Quality recommendations that PAC shows as errors. They fail
no target, so under the default result basis (`--target auto`) and with explicit targets they do not affect the
exit status. With `--target any`, every error-severity finding counts, and these do too (see
[JSON_OUTPUT.md](JSON_OUTPUT.md#summary-and-exit-status)).

## Requirements per target

`catalog/matrix.json` lists the requirements of each target and classes each one by how far automated checks can
decide it:

| Class | Definition |
|---|---|
| automatic | the reference validators decide the requirement automatically |
| partial | the reference validators detect some failures; human evaluation is still required |
| manual | no automated reference check exists; human evaluation is required |
| unsupported | machine-checkable in principle (Matterhorn "M") or outside PAC's WCAG set, but neither reference validator checks it |

Every requirement of class partial, manual or unsupported appears in the target's `manual_required` list in every
report.

| Target | requirements | automatic | partial | manual | unsupported | manual_required |
|---|---:|---:|---:|---:|---:|---:|
| `pdfua-1` | 181 | 127 | 2 | 49 | 3 | 54 |
| `pdfua-2` | 112 | 67 | 1 | 40 | 4 | 45 |
| `wtpdf-1.0-accessibility` | 1724 | 1723 | 0 | 1 | 0 | 1 |
| `wtpdf-1.0-reuse` | 1711 | 1710 | 0 | 1 | 0 | 1 |
| `wcag-2.0-A` | 25 | 0 | 8 | 17 | 0 | 25 |
| `wcag-2.0-AA` | 38 | 0 | 10 | 28 | 0 | 38 |
| `wcag-2.0-AAA` | 61 | 0 | 10 | 28 | 23 | 61 |
| `wcag-2.1-A` | 30 | 0 | 8 | 22 | 0 | 30 |
| `wcag-2.1-AA` | 50 | 0 | 11 | 39 | 0 | 50 |
| `wcag-2.1-AAA` | 78 | 0 | 11 | 39 | 28 | 78 |
| `wcag-2.2-A` | 31 | 0 | 7 | 22 | 2 | 31 |
| `wcag-2.2-AA` | 55 | 0 | 10 | 39 | 6 | 55 |
| `wcag-2.2-AAA` | 86 | 0 | 10 | 39 | 37 | 86 |

- **PDF/UA-1** requirements are the failure conditions of the Matterhorn Protocol 1.1, plus ISO 14289-1 clauses
  that are checked but have no Matterhorn mapping. Matterhorn ids are secondary references; the assessment does not
  depend on them.
- **PDF/UA-2** requirements are the clauses of ISO 14289-2, together with the rules of veraPDF's `ua2` profile.
- **WTPDF 1.0**: the requirements are the rules of veraPDF's `wt1a` / `wt1r` profiles. The WTPDF requirements that
  need human judgement are not listed one by one; they are covered by a single `manual` entry per level.
- **WCAG**: the criteria come from the W3C WCAG 2.2 data, with each criterion's level in 2.0, 2.1 and 2.2. No
  success criterion can be fully decided by a machine, so no WCAG requirement is `automatic`, and every criterion
  of a WCAG target is in its `manual_required` list.

WCAG success criteria with automated checks (class partial). Automated rules can show that a document fails these
criteria, but never that it meets them:

| SC | Title | Level | WCAG versions | mapped rules |
|---|---|---|---|---:|
| 1.1.1 | Non-text Content | A | 2.0, 2.1, 2.2 | 5 |
| 1.3.1 | Info and Relationships | A | 2.0, 2.1, 2.2 | 24 |
| 1.4.1 | Use of Color | A | 2.0, 2.1, 2.2 | 1 |
| 1.4.3 | Contrast (Minimum) | AA | 2.0, 2.1, 2.2 | 3 |
| 1.4.11 | Non-text Contrast | AA | 2.1, 2.2 | 1 |
| 2.4.2 | Page Titled | A | 2.0, 2.1, 2.2 | 3 |
| 2.4.3 | Focus Order | A | 2.0, 2.1, 2.2 | 1 |
| 3.1.1 | Language of Page | A | 2.0, 2.1, 2.2 | 4 |
| 3.1.2 | Language of Parts | AA | 2.0, 2.1, 2.2 | 7 |
| 4.1.1 | Parsing (Obsolete and removed) | A | 2.0, 2.1 | 136 |
| 4.1.2 | Name, Role, Value | A | 2.0, 2.1, 2.2 | 1 |

"Mapped rules" counts the rules the coverage matrix lists for the criterion, with any obligation. For 1.4.1 and
1.4.11 these are the review warnings above, so these two criteria can never fail automatically.

## Assessment model

Each report assesses 13 targets independently: `pdfua-1`, `pdfua-2`, `wtpdf-1.0-accessibility`, `wtpdf-1.0-reuse`
and the nine WCAG targets `wcag-{2.0,2.1,2.2}-{A,AA,AAA}`. Every target is assessed for every file, whatever the
PDF version or the conformance the file claims. A result for one target never implies anything about another:
PDF/UA conformance is not inferred from WCAG, and WCAG is not inferred from PDF/UA.

Because a file is checked against every target, it normally fails some of them. A file can conform to only one
PDF/UA part, and `pdfuaid:part` must be 1 for PDF/UA-1 and 2 for PDF/UA-2, so a file always fails one of the two
parts. The overall result and the exit status are therefore decided by a few *deciding targets*
(`--target`):

- `auto` (default): the PDF/UA part the document declares (`pdfuaid:part` 2 means `pdfua-2`; otherwise `pdfua-1`),
  together with `wcag-2.1-AA`, the WCAG level that EN 301 549 V3.2.1 references.
- A list of targets.
- `any`: every error-severity finding.

The choice affects only `summary` and the exit status. Assessments and findings are the same on every basis.

**Status of a target.** The rules are applied in this order:

1. `fail`: at least one error-severity finding of a rule with a normative mapping to the target.
2. `incomplete`: at least one required rule of the target was not evaluated or could not be decided, or the file
   could not be processed.
3. `automated_pass_manual_required`: otherwise. This is the best status an automated check can give. The
   requirements in `manual_required` must still be evaluated by a person.

There is no `pass` status. Warnings and recommendations never fail a target; they are listed in `warning_rules`.
Anything that stops the validator from reading part of the document completely makes the affected rules
incomplete: undecodable streams, the operator budget, a damaged file that qpdf had to reconstruct, internal limits.
A rule is never counted as passed on content that was not seen.

**WCAG versions and levels.** A rule mapped to success criterion *x* belongs to every WCAG version that contains
*x*, at every target level at or above *x*'s level in that version. WCAG conformance at a level includes all
criteria of lower levels. Examples:

- A rule mapped to 1.3.1 (level A in all versions) belongs to all nine WCAG targets.
- A rule mapped to 1.4.3 (level AA) belongs to `wcag-2.x-AA` and `wcag-2.x-AAA`, but not to `wcag-2.x-A`.
- A rule mapped to 4.1.1 (in 2.0 and 2.1, removed in 2.2) belongs to the 2.0 and 2.1 targets only.
- A rule mapped to 1.4.11 (new in 2.1) does not belong to the 2.0 targets.

**Combined statements.** For convenience, each report also combines `pdfua-1` and `pdfua-2` with each of the nine
WCAG targets (18 statements, e.g. `pdfua-1+wcag-2.1-AA`). `both` means that neither target has an automated
failure. It is not a conformance claim. If one target fails and the other is incomplete, the statement is
`pdfua_fail_wcag_incomplete` or `wcag_fail_pdfua_incomplete`, so a known failure is not hidden behind
`undetermined`. The full table is in [JSON_OUTPUT.md](JSON_OUTPUT.md#combined-statements).

## EN 301 549

Chapter 10 of EN 301 549 V3.2.1 (non-web documents) applies the WCAG 2.1 level A and AA success criteria to
documents. Clause 10.x.y.z corresponds to WCAG 2.1 SC x.y.z. Four WCAG 2.1 criteria are void for non-web documents
(10.2.4.1 Bypass blocks, 10.2.4.5 Multiple ways, 10.3.2.3 Consistent navigation, 10.3.2.4 Consistent
identification). Clauses 10.5 (caption positioning) and 10.6 (audio description timing) are advisory and have no
WCAG counterpart.

`dox-validate` has no separate EN 301 549 target. The clause mapping in `catalog/matrix.json` (`en301549`) is
derived from the WCAG 2.1 mapping, and `wcag-2.1-AA` is one of the two deciding targets of the default result
basis. The mapping has 48 clauses, 11 of them partial and 37 manual. The partial clauses are 10.1.1.1,
10.1.3.1, 10.1.4.1, 10.1.4.3, 10.1.4.11, 10.2.4.2, 10.2.4.3, 10.3.1.1, 10.3.1.2, 10.4.1.1 and 10.4.1.2. In practice:

- `wcag-2.1-AA` with status `fail` means that the document also fails the corresponding EN 301 549 clause 10
  requirements.
- `wcag-2.1-AA` with status `automated_pass_manual_required` means only that no automated check failed. Most of
  chapter 10, and every other chapter of EN 301 549, still needs a person to evaluate it.

## What is not covered

Automated checks examine what can be decided from the file. They do not judge meaning or quality. Among others,
`dox-validate` does not decide:

- whether alternative text, `ActualText`, titles, link texts, labels and headings are accurate or meaningful. It
  checks that they are present, attached to the right elements and in the right syntax;
- whether content tagged as real content should be an artifact, or the other way round (Matterhorn 01-001 and
  01-002);
- whether the reading order of the structure tree is the logical reading order (WCAG 1.3.2), or whether the
  structure types fit the content, for example whether something tagged as a paragraph is really a heading or a
  list. Structure is checked for validity (containment, nesting, role mapping, table regularity), not for
  semantics;
- whether the declared languages are the languages actually used. It checks that a language is declared and that
  the tag is syntactically valid;
- colour contrast on complex backgrounds, in images, or in CMYK or spot colours. These are reported as review
  warnings at most. The use of colour (1.4.1) and non-text contrast (1.4.11) are review hints only;
- multimedia, time-based content, keyboard operation and focus behaviour of interactive documents, error
  identification in forms, and the other criteria classed manual;
- the WTPDF requirements that need human judgement (not inventoried), and other formats such as PDF/A;
- clauses of EN 301 549 outside chapter 10, and any legal, procurement or certification requirement.

In addition:

- PAC 2026 conditions are reconstructed (see above). veraPDF behaviour is reproduced from its 1.30.2 sources and
  checked against its results, but differences on unusual files are possible.
- A damaged file is validated as reconstructed by qpdf. The repairs are listed in `diagnostics`, `file_repaired`
  is `true`, and every rule is marked incomplete, so the file cannot reach an automated pass.
