# Correctness analysis of dox-validate on the test corpus

Automated checks cannot establish conformance with PDF/UA or WCAG; this analysis only asks whether the machine-checkable
findings of `dox-validate` agree with independently established expectations, and explains every disagreement found.
Independent expectations and tool observations are kept apart throughout.

## 1. Method

- **Validator:** `dox-validate` 0.2.0, built from commit `f3c5993` (Release), catalogue sha256 `6938a041...`, for the
  tool comparison (sections 3 and 4). Commit `f37a695` fixed D1 and resolved D2 without changing any other finding
  (the maintainer verified whole-report identity apart from that rule); the test suite was re-run against it.
- **Reference tool:** veraPDF 1.30.2 greenfield (the release whose rule semantics the `EQX-UA1-*`, `EQX-UA2-*`,
  `EQX-TS32005-*` and `EQX-WTPDF-*` rules reproduce) with the `ua1` and `ua2` flavours. Version questions were checked
  with veraPDF 1.30.4. Both are the official veraPDF release packages. PAC is not available on Linux, so the
  `EQX-PAC-*`/`EQX-PACQ-*` rules are compared with the fixture expectations only.
- **Corpus:** `build_corpus.py --suites`, 554 files:
  - 84 synthetic fixtures and 6 generated size tiers;
  - 29 real-world documents;
  - the complete veraPDF PDF/UA-1 (297 files) and PDF/UA-2 (138 files) test suites at commit `bb75f4f`.
- **Expectations:**
  - **Fixtures:** each expectation follows from how the file was built and from the cited clause. It is recorded in
    `fixtures/expected.json` with the ISO 14289-1/-2 clause, the Matterhorn condition or the WCAG success criterion.
  - **veraPDF suite:** the corpus file name states the verdict for one test (`<clause>-t<n>-<pass|fail>-x.pdf`). That
    is a statement by the test-suite authors.
  - **Real-world documents:** only the publishers' own claims are recorded, such as a pdfuaid declaration. Those claims
    are not verified.
- **Inspection:** disagreements were inspected with `qpdf --qdf` / `--show-object`, `pdffonts` and fontTools (CFF
  charstrings), and with dox-validate's `--reference-dedup` mode.

## 2. Synthetic fixtures (construction-derived expectations)

All 84 fixtures meet their expectations as of `f37a695`; the documented deviations below are allowed. At `f3c5993`
one fixture was affected by defect D1 (section 5). The checks cover the expected findings, the rules that must not be
reported, the absence of unexpected rules within each fixture's `exclusive_prefixes`, the exit status and
`summary.result` under both result bases (the default `auto`, which judges the declared PDF/UA part and wcag-2.1-AA,
and `--target any`), the deciding targets, and the target statuses. Further coverage:

- **Generated tiers:** all 6 meet their expectations as of `f37a695`. The 3 complex tiers (100, 500 and 2000 pages,
  tagged and PDF/UA-1-clean by construction) exit 0 under the default basis; at `f3c5993` they were affected by D1.
- **On-the-fly cases:** 14 cases built inside the tests (`test_contrast_cases.py`, `test_structure_cases.py`) all pass.
  They cover:
  - WCAG 1.4.3 thresholds: 4.54:1 passes and 4.48:1 fails at 11 pt; 3.03:1 passes at 18 pt and at 14 pt bold, and fails
    at 14 pt regular and at 13 pt bold;
  - text on opaque backgrounds, and invisible or artifact text;
  - ColSpan and RowSpan;
  - role-mapped headings;
  - nested lists.

Three fixtures trigger a rule that, on our reading of the normative text, does not apply. In each case veraPDF 1.30.2
reports the same rule on the same file, so dox-validate reproduces its reference semantics as designed. These cases are
recorded as `known_deviations` and classified as **(b) reference-tool behaviour, reproduced**.

| fixture | rule | construction | analysis |
|---|---|---|---|
| `artifact-in-tagged` | UA1 7.1-2 (tagged content inside an artifact) | an `/Artifact BMC` nested in a P's marked content; the artifact holds only untagged text | ISO 14289-1 7.1 is violated as "artifact inside tagged content" (7.1-1, also reported). No tagged content lies inside the artifact. |
| `structelem-no-parent` | UA1 7.1-3 (content neither tagged nor artifact) | a P without `/P` that is reachable from the root by `/K` and owns its MCIDs through the ParentTree | The content is tagged. The missing `/P` is the actual violation (ISO 32000-1 Table 323), which dox-validate reports as 7.1-12. veraPDF logs "P key ... different from the actual parent" and drops the element. |
| `font-truetype-no-encoding` | UA1 7.21.4.1-2 (glyph missing) | non-symbolic embedded TrueType with only a (3,1) cmap and no `/Encoding` | Under ISO 32000-1 9.6.6.4 the codes map through StandardEncoding and the Adobe Glyph List to (3,1), and every glyph used exists. The missing `/Encoding` itself is correctly reported as 7.21.6-2. |

Two first constructions were wrong and were corrected before freezing; neither was a validator issue:

- The TrapNet annotation first had `/Rect [0 0 0 0]`, which both tools treat as outside the crop box and exempt.
- The XFA form first set only `/NeedsRendering`. veraPDF and dox-validate detect a dynamic form through the XFA config
  `dynamicRender`. That is a scope limit, listed in section 6.

## 3. veraPDF test suites (independent expectation: the file name)

Tally per suite:

| suite | fail files detected | pass files without the finding | fail files not detected |
|---|---|---|---|
| PDF/UA-1, 297 files | 143 / 156 | 141 / 141 | 13 |
| PDF/UA-2, 138 files | 83 / 84 | 54 / 54 | 1 |

On every one of these files, dox-validate's verdict equals that of veraPDF 1.30.2. A finding counts only when
`non_pac_occurrences` > 0, so the veraPDF condition of a merged rule must have fired.

The 14 undetected fail files break down as follows:

- **Numbering, 9 files** (`suite_disputes.json`, classified **(d)**). The file violates a neighbouring test of the same
  clause, and dox-validate, veraPDF 1.30.2 and veraPDF 1.30.4 all report that neighbour:
  - 7.2-t03 is reported as 7.2-39 or 7.2-16;
  - 7.2-t19 as 7.2-40;
  - 7.2-t27 as 7.2-28;
  - 7.2-t41 as 7.2-42;
  - 7.1-t05-fail-d as 7.1-6;
  - UA-2 8.7-t02 as 8.7-1.
  
  The violation is found; only the test number differs.
- **Version, 1 file: `7.5-t01-fail-c`** (classified **(c)**). veraPDF 1.30.4 reports 7.5-1; veraPDF 1.30.2 and
  dox-validate report nothing. Adopting the 1.30.4 semantics would close it.
- **Coverage, 4 files: 7.18.7-t01** (classified **(c)**). Clause 7.18.7 has no rule in the catalogue (and none in the
  veraPDF 1.30.2 profile), so nothing is reported. The files fail 7.11-1 instead, which both tools report.

## 4. Rule-level comparison with veraPDF 1.30.2

veraPDF parsed 545 of the 554 files. The 9 it rejects are the malformed, zero-page and user-password fixtures;
dox-validate reports 8 of them as not processed and `malformed-bad-xref` as incomplete (exit 2).

Over the 545 parsed files, every veraPDF rule failure (ua1 and ua2 flavours, including the 1,636 ISO/TS 32005
containment rules) maps through the catalogue to a dox-validate finding. **There is no case where veraPDF fails a rule
and dox-validate does not.** dox-validate reports these extra failures:

| rule(s) | files | evidence | class |
|---|---|---|---|
| UA1 7.1-12, UA2 8.2.1-2 (no `/P`) | `structelem-no-parent` | The construction omits `/P`, which ISO 32000-1 Table 323 requires. veraPDF does not report it. | dox-validate correct; it differs from the claimed 1.30.2 semantics in the stricter direction |
| UA1 7.21.5-1, UA2 8.4.5.6-1 (glyph width) | `govinfo-budget-2025-app` p. 1258 | `TimesNewRomanPSStd-Italic` (obj 9199): `/Widths` gives 250 for code 0x20, but the embedded CFF charstring for `space` is `endchar` without a width, so its advance is `defaultWidthX` = 500. The finding disappears with `--reference-dedup`: veraPDF checks a reused content stream only at its first use. | (b) reference-tool limitation; dox-validate correct |
| UA2 8.10.3.5-1 (signature graphic without alternative text) | 3 govinfo documents | A visible `/FT /Sig` widget (GPO seal, `/F 132`) paints vector graphics; there is no structure and no `/Contents`. The source comment documents that veraPDF 1.30.2 can never fail this rule, so dox-validate evaluates ISO 14289-2 8.10.3.5 itself. | (c) designed scope difference; dox-validate is normatively stricter |

## 5. Suspected validator defects and reporting issues

| id | severity | summary | status |
|---|---|---|---|
| D1 | low (warning-level) | WCAG 1.4.1 review heuristic: false positives next to underlined links | fixed in `f37a695` |
| D2 | medium (CLI contract) | exit status 0 is unreachable for any processed file | resolved in `f37a695` by the result basis `--target` |
| D3 | low (reporting) | `EQX-UA1-7.2-29` is reported for a missing Lang | open, documented |
| D4 | low (documentation) | `--rules`: exit status relative to the selection | documented behaviour of `--target any`, the default with `--rules` |
| D5 | cosmetic | "syntax error" wording for a well-formed tag | open, documented |
| D6 | cosmetic | whitespace-`Contents` warning raised for a missing `/Contents` | open, documented |

**D1 (low, warning-level): WCAG 1.4.1 review heuristic gives false positives next to underlined links**
(`known_defects.json`).
- *Repro:* `dox-validate --json tests/corpus/fixtures/link-no-contents.pdf` reports `EQX-WCAG-1.4.1-UseOfColorReview`
  with the detail `"Read:" (#000000) is set apart from the adjacent text (#000099) by colour alone ... no underline`.
  The same happens on every page of `gen-*-complex`.
- *Construction:* black body text, followed by a blue link that is underlined by a stroked path. The link is the only
  coloured text and it has a non-colour cue (WCAG 2.x 1.4.1, failure F73 does not apply).
- *Cause:* `colour_page` (`src/rules/pac_quality/wcag_color.cpp`) takes the most frequent colour per font and size on
  the page as the reference. When the link text outweighs the 11 pt black text, the black text becomes the "deviant"
  phrase. The underline test then looks for an underline below that black text, not below the link.
- *Expected:* no finding, or a finding about the link without the false "no underline" claim.
- *Fix (`f37a695`):* a phrase counts as underlined when thin baseline paths together cover at least 60 % of it, and no
  finding is made when the adjacent reference-colour phrase is underlined or is a framed link. `link-no-contents` and
  the complex tiers now have no finding of this rule (`must_not_report`), and it is no longer a known defect.

**D2 (medium, CLI contract): exit status 0 is unreachable for any processed file.**
- *Observation:* on all 554 corpus files the exit status is 1 or 2, and `summary.result` is never `no_errors_found`.
  This includes `ua1-good` and `ua2-good`, which have no automated failure for their own PDF/UA part and the WCAG targets.
- *Cause:* `summarize()` counts the error findings of all 13 targets. ISO 14289-1 clause 5 needs `pdfuaid:part` 1
  (`EQX-UA1-5-2`), and ISO 14289-2 clause 5 needs part 2 (`EQX-UA2-5-2`); a missing declaration fails `EQX-UA1-5-1`.
  So every processed file has an error-severity finding.
- *Effect:* exit 1 carries no information. The meaningful verdicts are only in `assessments`.
- *Suggestion:* let the exit status follow selected targets (e.g. the declared part or a `--target` option), or
  document this explicitly. Only `--rules` could give exit 0.
- *Resolution (`f37a695`):* `--target LIST` (`auto`, `any`, or target names) decides `summary.result` and the exit
  status. The default `auto` uses the declared PDF/UA part (`input.pdfua_part`; 2 gives pdfua-2, anything else pdfua-1)
  and wcag-2.1-AA; `summary.basis`, `targets`, `failed_targets` and `incomplete_targets` report it. `ua1-good`,
  `ua2-good` and 15 other fixtures now exit 0. `--target any` keeps the former semantics, and is the default under the
  legacy name `eqx-validate` and with `--rules`. `test_target.py` and the per-basis invariants
  (`report_invariants.check_basis`) cover it; findings and assessments are identical under every basis.

**D3 (low, reporting): `EQX-UA1-7.2-29` is reported for a missing Lang.**
- *Repro:* `lang-missing` and `edge-incremental-update` report 7.2-29 with the detail "PAC: document language missing".
  Its `non_pac_occurrences` is 0.
- *Analysis:* the rule title is "Lang values must be well-formed language tags". The
  catalogue merges PAC's "document language" condition into this veraPDF rule. The missing language is correctly
  reported by 7.2-34, 7.2-33 and `EQX-UA2-8.4.4-1`; the extra 7.2-29 entry contradicts its own title.
- *Suggestion:* report PAC's "missing" case under its own id. Consumers comparing with veraPDF must use
  `non_pac_occurrences`.

**D4 (low, documentation): `--rules` exit status is relative to the selection.**
- *Observation:* `--rules EQX-WCAG-` on `contrast-black.pdf` exits 0 with `no_errors_found`, while about 2,000 required
  rules stay unevaluated. The targets stay `incomplete`, which is correct.
- *Conflict:* `--help` defines exit 0 as "every required rule evaluated". The code (`summarize`, `in_selection`)
  intends exit 0 to mean every rule in the selection.
- *Suggestion:* state this in `--help`. `test_options.py` pins the current behaviour.
- *Status (`f37a695`):* `--rules` now defaults to `--target any`, whose documented meaning is "every required rule of
  the evaluation"; the deciding-target bases keep the targets `incomplete`, which is the safe reading.

**D5 (cosmetic): "syntax error" wording for a well-formed tag.**
- *Observation:* `lang-unknown-primary` (`/Lang (zz-ZZ)`) is reported as "PAC: document language 'zz-ZZ' syntax
  error". The tag is well-formed BCP 47; its primary subtag is unregistered.
- *Analysis:* the verdict is right. RFC 3066, which ISO 32000-1 14.9.2 cites, requires an ISO 639 primary subtag. Only
  the wording is wrong. `EQX-PAC-FailValidLanguagePrimaryTagOther` is deliberately never raised: according to the code
  comment, PAC 26.1 reports such tags as syntax errors.

**D6 (cosmetic): whitespace-`Contents` warning for a missing `/Contents`.**
- *Observation:* `EQX-PAC-WarnContentsEntryOnAnnotationIsWhiteSpace` ("Contents entry ... exists, but is only
  whitespace") is raised for a tagged Link with no `/Contents` at all (`link-no-contents`).
- *Analysis:* this is intentional emulation of the observed PAC behaviour (source comment "U1-A01"). The title is
  misleading for this case.

The robustness checks found no crash, hang or excessive memory use. They covered:

- 13 malformed, edge and encrypted fixtures;
- 30 deterministic mutations (truncation, byte flips, inserted garbage, dropped keywords, 100k-deep arrays, 50k-deep
  dictionaries, self-referencing page trees);
- `--max-memory-mb 48` on the 2000-page tier;
- all 554 corpus files.

Exit statuses were always 0, 1 or 2, every report validated against the schema, and peak RSS stayed below 1 GiB.

## 6. Scope limits (not defects)

- **Dynamic XFA:** a form flagged only by `/NeedsRendering true` is not recognised. UA1 7.15-1 follows veraPDF and
  looks at the XFA config `dynamicRender`; the UA2 8.10.1-3 rule still reports the XFA itself.
- **TrapNet exemption:** annotations whose rectangle is outside the crop box are exempt from 7.18.2-1 (TrapNet). ISO
  14289-1 states that exemption only for 7.18.1.
- **Images of text:** `scanned-tagged-figure`, an image of text tagged as one Figure with Alt, meets every
  machine-checkable PDF/UA-1 condition and gets `automated_pass_manual_required`. Rejecting it needs the manual check
  (WCAG 1.4.5 Images of Text, PDF/UA 7.3).
- **PAC rules:** `EQX-PAC-*` and `EQX-PACQ-*` emulate PAC 26.1 and were checked here only against construction-derived
  expectations (headings, ParentTree, BBox, outline, language, tables, contrast), not against PAC itself.
- **Missing reference suite:** the PDF Association PDF/UA Reference Suite could not be downloaded non-interactively
  (HTTP 403 bot challenge), so it is not part of the corpus.

## 7. Reproducing

```sh
python3 tests/corpus/build_corpus.py --suites --out /tmp/corpus     # 554 files
verapdf --flavour ua1 --format mrr FILE...                         # veraPDF 1.30.2 greenfield release
build/dox-validate --format json FILE...                            # compare via the catalogue's veraPDF sources
.venv/bin/python -m pytest tests                                    # fixtures, suites and public documents
```
