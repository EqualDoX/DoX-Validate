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

Two fixtures trigger a rule that, on our reading of the normative text, does not apply. In each case veraPDF 1.30.2
reports the same rule on the same file, so dox-validate reproduces its reference semantics as designed. These cases are
recorded as `known_deviations` and classified as **(b) reference-tool behaviour, reproduced**.

| fixture | rule | construction | analysis |
|---|---|---|---|
| `structelem-no-parent` | UA1 7.1-3 (content neither tagged nor artifact) | a P without `/P` that is reachable from the root by `/K` and owns its MCIDs through the ParentTree | The content is tagged. The missing `/P` is the actual violation (ISO 32000-1 Table 323), which dox-validate reports as 7.1-12. veraPDF logs "P key ... different from the actual parent" and drops the element. |
| `font-truetype-no-encoding` | UA1 7.21.4.1-2 (glyph missing) | non-symbolic embedded TrueType with only a (3,1) cmap and no `/Encoding` | Under ISO 32000-1 9.6.6.4 the codes map through StandardEncoding and the Adobe Glyph List to (3,1), and every glyph used exists. The missing `/Encoding` itself is correctly reported as 7.21.6-2. |

A third deviation of this kind was removed on 2026-11-18 (D7 in section 5). `artifact-in-tagged`, an `/Artifact BMC`
nested in a P's marked content, used to report UA1 7.1-2 (tagged content inside an artifact) next to 7.1-1, as veraPDF
does. No tagged content lies inside the artifact, so the fixture now expects 7.1-1 and must not report 7.1-2;
`tagged-in-artifact` expects the opposite.

Two first constructions were wrong and were corrected before freezing; neither was a validator issue:

- The TrapNet annotation first had `/Rect [0 0 0 0]`, which both tools treat as outside the crop box and exempt.
- The XFA form first set only `/NeedsRendering`. veraPDF and dox-validate detect a dynamic form through the XFA config
  `dynamicRender`. That is a scope limit, listed in section 6.

## 3. veraPDF test suites (independent expectation: the file name)

Tally per suite:

| suite | fail files detected | pass files without the finding | fail files not detected |
|---|---|---|---|
| PDF/UA-1, 297 files | 144 / 156 | 141 / 141 | 12 |
| PDF/UA-2, 138 files | 83 / 84 | 54 / 54 | 1 |

On every one of these files, dox-validate's verdict equals that of veraPDF 1.30.2, except `7.5-t01-fail-c`, where it
follows veraPDF 1.30.4 (see below). A finding counts only when
`non_pac_occurrences` > 0, so the veraPDF condition of a merged rule must have fired.

Since 2026-11-18 the pass files are also without the finding through any definition, PAC's included
(`test_verapdf_pass_file_is_not_reported_through_any_definition`). Seven PDF/UA-1 pass files used to be reported under
their rule through a merged PAC condition alone: `7.2-t29-pass-a` to `-f` (D5) and `7.18.1-t03-pass-d` (D8). One file
changed on the fail side without changing the tally: `7.1-t01-fail-a` reports 7.1-1, the test it is named after, and
no longer 7.1-2 as well (D7).

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
- **Version, 1 file: `7.5-t01-fail-c`, resolved.** veraPDF 1.30.4 reports 7.5-1 and veraPDF 1.30.2 reports nothing.
  dox-validate now follows 1.30.4 here. For PDF/UA-1, a TD of a table with a TH that has no Scope counts as connected
  only through its `Headers`, with no search for headers by position (upstream `GFSETable.hasHeaders`). It reports this
  file, and no other file of the corpus changed.
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
| UA1 7.3-1 through PAC's definition only (`non_pac_occurrences` 0) | `libreoffice-draw-guide-268` | The RoleMap remaps the standard type `/Figure` to `/P`, which 7.1-7 reports 1,092 times. veraPDF then checks the 1,090 Figures without `/Alt` or `/ActualText` as P and asks for no alternative text. PAC takes a standard `/S` as is, as dox-validate's PAC model does for its other Figure checks, so dox-validate reports them as PAC-equivalent findings. VellumPdf 2.3.3 and outloud 0.2.1 also report them. | PAC-equivalent scope; the veraPDF-definition count is unchanged |
| UA2 8.10.3.5-1 (signature graphic without alternative text) | 3 govinfo documents | A visible `/FT /Sig` widget (GPO seal, `/F 132`) paints vector graphics; there is no structure and no `/Contents`. The source comment documents that veraPDF 1.30.2 can never fail this rule, so dox-validate evaluates ISO 14289-2 8.10.3.5 itself. | (c) designed scope difference; dox-validate is normatively stricter |

## 5. Suspected validator defects and reporting issues

| id | severity | summary | status |
|---|---|---|---|
| D1 | low (warning-level) | WCAG 1.4.1 review heuristic: false positives next to underlined links | fixed in `f37a695` |
| D2 | medium (CLI contract) | exit status 0 is unreachable for any processed file | resolved in `f37a695` by the result basis `--target` |
| D3 | low (reporting) | `EQX-UA1-7.2-29` is reported for a missing Lang | fixed 2026-11-18: PAC's condition has its own rule |
| D4 | low (documentation) | `--rules`: exit status relative to the selection | documented behaviour of `--target any`, the default with `--rules` |
| D5 | medium (false positive) | a well-formed tag with an unregistered primary subtag is reported as a 7.2-29 "syntax error" and fails PDF/UA-1 | fixed 2026-11-18; the earlier analysis ("cosmetic, the verdict is right") is withdrawn |
| D6 | cosmetic | whitespace-`Contents` warning raised for a missing `/Contents` | open, documented |
| D7 | low (reporting) | an Artifact nested in tagged content is reported under 7.1-2 as well as 7.1-1 | fixed 2026-11-18 (a deliberate difference from veraPDF) |
| D8 | medium (false positive) | a form field without area beyond the crop box is reported as a field without a name | fixed 2026-11-18 |
| D9 | low (reporting) | the content of a Private element in a table row is reported as misplaced a second time | fixed 2026-11-18 |
| D10 | low (false positive) | a header cell with an empty ID that a Headers entry names is reported as not associated | fixed 2026-11-18 |

D3, D5 and D7 to D10 were found, or found again, by a detection-quality benchmark of six validators on 984 real-world
documents, the veraPDF suites and one controlled case per rule (2026-11-16 to 18, kept outside the repository). Each
has regression tests built on the fly (`test_language_cases.py`, `test_artifact_cases.py`, `test_annotation_cases.py`,
`test_structure_cases.py`) and unit tests (`document_lang_test.cpp`, `content_model_test.cpp`, `pac_structure_model.cpp`).

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
- *Fix (2026-11-18):* the case is reported as `EQX-PAC-FailValidLanguageSyntax` ("document language missing"), an error
  finding that fails WCAG 3.1.1 and does not fail PDF/UA-1. ISO 32000-1 14.9.2.1 says the document language "may be
  specified" in the catalog, and ISO 14289-1 7.2 asks that the language of every text can be determined, which
  7.2-34, 7.2-2, 7.2-33 and the other language rules check. A document without a catalog Lang whose elements carry
  the language therefore meets PDF/UA-1: the 18 pass files of veraPDF's tests 7.2-21 to 7.2-34 that are built this
  way no longer fail it. `lang-missing` must now report the PAC rule and must not report 7.2-29.

**D4 (low, documentation): `--rules` exit status is relative to the selection.**
- *Observation:* `--rules EQX-WCAG-` on `contrast-black.pdf` exits 0 with `no_errors_found`, while about 2,000 required
  rules stay unevaluated. The targets stay `incomplete`, which is correct.
- *Conflict:* `--help` defines exit 0 as "every required rule evaluated". The code (`summarize`, `in_selection`)
  intends exit 0 to mean every rule in the selection.
- *Suggestion:* state this in `--help`. `test_options.py` pins the current behaviour.
- *Status (`f37a695`):* `--rules` now defaults to `--target any`, whose documented meaning is "every required rule of
  the evaluation"; the deciding-target bases keep the targets `incomplete`, which is the safe reading.

**D5 (medium, false positive): a well-formed tag with an unregistered primary subtag fails 7.2-29 and PDF/UA-1.**
- *Observation:* `lang-unknown-primary` (`/Lang (zz-ZZ)`) was reported under `EQX-UA1-7.2-29` as "PAC: document
  language 'zz-ZZ' syntax error". The benchmark found the same on real-world documents with `/Lang (Deutsch)` and
  `(English)`, on `PDFUA-Ref-2-09_Scanned.pdf` of the PDF Association's PDF/UA Reference Suite 1.1 (`English`; the
  suite's files are declared conformant), and on veraPDF's `7.2-t29-pass-a` to `-f` (`portugue`, `p`, ...).
- *Earlier analysis, withdrawn:* "the verdict is right. RFC 3066, which ISO 32000-1 14.9.2 cites, requires an ISO 639
  primary subtag. Only the wording is wrong."
- *Analysis:* the requirement is ISO 32000-1:2008 14.9.2.2: "A language identifier shall either be the empty text
  string, to indicate that the language is unknown, or a Language-Tag as defined in RFC 3066". RFC 3066 2.1 defines
  Language-Tag by a grammar: a primary subtag of 1 to 8 letters, then subtags of 1 to 8 letters or digits. That is
  what veraPDF's 7.2-29 tests, and each of these values meets it. RFC 3066 2.2 says which primary subtags have an
  assigned meaning and that other values "shall not be assigned except by revision of this standard"; it reserves
  them, and it has no notion of an invalid tag. The check that failed them is PAC's, and it is not RFC 3066's
  either: it asks for a well-formed BCP 47 tag whose primary subtag is in the IANA Language Subtag Registry, which
  RFC 4646 introduced after RFC 3066. So the registry lookup is a check of PAC and of WCAG 3.1.1 (W3C's ACT rule
  bf051a fails a page language without a known primary subtag), not a requirement of ISO 14289-1.
- *Fix (2026-11-18):* the cases are told apart. An unregistered primary subtag is
  `EQX-PAC-FailValidLanguagePrimaryTagOther`, which is raised now; a tag that meets the grammar but is not well-formed
  BCP 47 (`p`, `en-US-US`) is `EQX-PAC-FailValidLanguageSyntax`. Both, and `EQX-PAC-FailValidLanguagePrimaryTagX`
  (`x-...`, which RFC 3066 2.2 reserves for private use), stay error findings and fail the WCAG targets; for PDF/UA-1
  they are listed in `warning_rules`, and their inferred Matterhorn 11-006 reference is dropped. A value outside the
  grammar (`en_US`) fails 7.2-29 through both definitions, as before. `lang-unknown-primary` and `lang-private-use`
  now expect PDF/UA-1 to pass and WCAG to fail.
- *What this does not settle:* whether a tag that names no language lets the language "be determined" in the sense
  of ISO 14289-1 7.2 is a matter of reading. PAC reads it as a PDF/UA failure; veraPDF and the Reference Suite do not.
  The finding is reported either way; only its effect on the `pdfua-1` status changed.

**D7 (low, reporting): an Artifact nested in tagged content was reported under 7.1-2 as well.**
- *Observation:* `artifact-in-tagged`, veraPDF's `7.1-t01-fail-a` and four real-world documents of the benchmark
  (whole pages wrapped in `/Document <</MCID 0>> BDC` with Artifacts inside; a form with Artifacts painted inside a
  Figure's sequence) reported `EQX-UA1-7.1-2` once per Artifact and once per sequence inside it, next to 7.1-1.
- *Analysis:* the Matterhorn Protocol has two machine-checkable conditions under ISO 14289-1 7.1: 01-003 "Content
  marked as Artifact is present inside tagged content" and 01-004 "Tagged content is present inside content marked as
  Artifact". They name the two ways of nesting. veraPDF's 7.1-2 tests `parentsTags.contains('Artifact')`, and that
  list holds the sequence's own tag, so every Artifact that 7.1-1 reports fails 7.1-2 too; the opposite nesting fails
  7.1-2 alone. PAC reports only the nesting that is there.
- *Fix (2026-11-18):* 7.1-2 is reported where content becomes tagged content inside an Artifact
  (`ContentModel::tagged_from_inside_artifact`). The documents fail PDF/UA-1 as before, through 7.1-1. This is a
  deliberate difference from veraPDF, also under `--reference-dedup`. On the 3,407 files of the benchmark the eight
  that still report 7.1-2 do so with unchanged counts.

**D8 (medium, false positive): a form field without area beyond the crop box.**
- *Observation:* veraPDF's `7.18.1-t03-pass-d` (a text field without TU, `/Rect [800 800 800 800]` on a 612 x 792 page)
  was reported under `EQX-UA1-7.18.1-3` as "PAC: form field with an empty /Rect and no /TU". PAC 26.1 fails the file too.
- *Analysis:* ISO 14289-1 7.18 does not apply to annotations whose rectangle is outside the crop box (Matterhorn
  Protocol, checkpoint 28). The PAC-derived condition exists for fields without area that veraPDF's inclusive
  comparison counts as outside although they lie on the page (`[0 0 0 0]`, invisible signatures); it did not look at
  where the rectangle is.
- *Fix (2026-11-18):* the condition applies when the point of the rectangle lies in the crop box, its edge included.

**D9 (low, reporting): the content of a Private element in a table row.**
- *Observation:* for `TR(TD, TD, Private(P))` both `EQX-PAC-FailPrivateTagInvalidSyntax` and
  `EQX-PAC-FailPTagInvalidSyntax` ("P in Private (context TR)") were reported. PAC 26.1 reports the first only.
- *Fix (2026-11-18):* an element directly in a Private is not judged against the strict parent the Private stands in;
  the Private itself always is. NonStruct, which has no check of its own, is looked through as before.

**D10 (low, false positive): a header cell with an empty ID.**
- *Observation:* veraPDF's `7.5-t01-pass-d` (`TH /ID ()`, named by `/Headers [()]`) was reported under
  `EQX-PAC-THHasAssociatedCells_...` as "TH without Scope and not named by Headers". PAC 26.1 passes the file.
- *Fix (2026-11-18):* an empty ID counts when a Headers entry names it. `7.5-t01-pass-c` and `-e`, whose first header
  cell is named by no cell, are still reported, as PAC does.

**D6 (cosmetic): whitespace-`Contents` warning for a missing `/Contents`.**
- *Observation:* `EQX-PAC-WarnContentsEntryOnAnnotationIsWhiteSpace` ("Contents entry ... exists, but is only
  whitespace") is raised for a tagged Link with no `/Contents` at all (`link-no-contents`).
- *Analysis:* this is intentional emulation of the observed PAC behaviour (source comment "U1-A01"). The title is
  misleading for this case.

**Open observations on real-world documents (2026-11-18, not fixed).** After the fixes above, the old and the new
binary were compared on 1,000 real-world documents that the benchmark had not evaluated, drawn at random, with ground
truth from the benchmark's structural oracle and no adjudication. Both binaries score the same there: 5,975 of 5,979
violated requirements detected, 6 of 46,109 satisfied requirements reported. None of the six comes from a rule changed
above. They are listed here as they were found; each needs the review that the benchmark's own disagreements got.

| observation | rule | what the document has | reading |
|---|---|---|---|
| Popup annotations | UA1 7.18.1-1, 7.18.1-2 | visible `/Popup` annotations without a structure parent; their markup annotations are hidden | The Matterhorn Protocol 1.1 exempts annotations "whose Subtype is Popup" from clause 7.18 (checkpoint 28). veraPDF's tests name no such exemption, and dox-validate reproduces them. Probably a false positive. |
| empty text in an unembedded font | UA1 7.21.4.1-1 (2 documents) | appearance streams of empty text fields: `/Helv 9.7 Tf ... () Tj` with Helvetica or Times-Roman not embedded | No glyph is rendered, so the font is arguably not "used for rendering". Probably a false positive. |
| remapped standard type | UA1 7.2-4 | RoleMap `/TBody` -> `/P`; the rows are in a TBody | The remapping is the violation (7.1-7, reported). 7.2-4 follows from applying it, as veraPDF does; a consequence of the same defect. |
| language alternative without `xml:lang` | UA1 7.1-9 | `<dc:title><rdf:Alt><rdf:li>...</rdf:li></rdf:Alt></dc:title>` | The title is there, the language alternative is malformed (ISO 16684-1). A matter of reading. |

The four requirements missed there are differences of reading that the benchmark documents: a `/TU` or `/Contents`
of white space only counts as present, as in veraPDF's tests (3), and content whose MCID leads to a structure element
that the tree does not reach from the root counts as tagged (1).

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

## 7. Comparison with VellumPdf 2.3.3 and outloud 0.2.1

Both tools are PDF/UA-1 checkers with engines of their own:

- **VellumPdf.Cli** (`vellum-preflight -p ua1`) has 106 rules that mirror veraPDF's PDF/UA-1 profile.
- **outloud** has a conformance layer and a heuristic "semantic" layer.

They ran on the 554 files of `build_corpus.py --suites` next to dox-validate (2026-11-14). Every error class that
either tool reports and dox-validate does not was examined against the standard and the PDF structure.

veraPDF PDF/UA-1 test suite, with the verdict taken from the file name. outloud is matched at clause level, because
its rule ids are its own.

| tool | fail files detected | pass files flagged |
|---|---|---|
| dox-validate | 144 / 156 | 0 / 140 |
| VellumPdf 2.3.3 | 128 / 156 | 20 / 140 |
| outloud 0.2.1 | 76 / 156 | 33 / 140 |

None of the suite fail files that dox-validate misses is detected by either tool under its expected test, except the
two cases below.

**Adopted**

- **Remapped Figures.** A Figure whose standard type the RoleMap remaps is now checked for alternative text through
  PAC's definition of 7.3-1. The case is `libreoffice-draw-guide-268`, which has 1,090 Figures; VellumPdf and outloud
  report them too. See section 4.
- **7.5-1 as in veraPDF 1.30.4.** VellumPdf misses `7.5-t01-fail-c`; outloud reports it. See section 3.

**Not adopted, with the reasons**

- **VellumPdf 7.2-41/42/43.** VellumPdf reports all three rules for every irregular table. dox-validate and veraPDF
  report the one variant that applies; the tables are the same (23 of 23 in `nist-sp800-53r5`).
- **VellumPdf 7.5-1, 895 times in `nist-sp800-53r5`.** These TDs are in tables that have no TH at all. veraPDF 1.30.2
  and 1.30.4 report nothing.
- **VellumPdf 7.18.1-3 on two US government forms.** veraPDF reports nothing, and VellumPdf also flags the suite's
  7.18.1-t03 pass files.
- **VellumPdf 7.1-5 next to 7.1-6 for a circular role map.** This is one defect reported twice. veraPDF reports 7.1-6,
  as dox-validate does.
- **VellumPdf missing language on an untagged scan.** dox-validate reports it through PAC's document-language rule
  (`EQX-PAC-FailValidLanguageSyntax`) and through UA-2 8.4.4-1.
- **outloud DOC-012.** outloud checks PDF/UA-1 only, so it fails `pdfuaid:part` 2.
- **outloud LNK-004 (Print flag) and DOC-015 (Info /Title vs XMP).** These are PDF/A requirements, not PDF/UA ones.
- **outloud DOC-021.** It flags XFA, but both files carry static XFA, which ISO 14289-1 7.15 allows.
- **outloud TXT-003, FRM-001, LNK-002, MATH-001.** These fire on suite pass files and on `field-alt-instead-of-tu`.
- **outloud table and list nesting (TBL-003/004/005, LST-001/003/004).** dox-validate reports the same defects
  under the 7.2-x containment rules.

**outloud's semantic layer.** These are heuristics, not PDF/UA conformance checks.

- **Already covered by dox-validate's PAC Quality equivalents:** a title that is a file name, prose marked as an
  artifact, URLs without a link, missing bookmarks, and whitespace-only text.
- **Not covered, candidates for review-only rules:** empty structure elements, headings that are paragraphs,
  image-only pages, placeholder alternative text, private-use Unicode mappings, references that point nowhere, and Link
  elements without text.
- **Too noisy on this corpus:** ruled grids without a Table (739 hits, mostly form boxes), the paint-order heuristic,
  and "shredded" header cells.

## 8. Reproducing

```sh
python3 tests/corpus/build_corpus.py --suites --out /tmp/corpus     # 554 files
verapdf --flavour ua1 --format mrr FILE...                         # veraPDF 1.30.2 greenfield release
build/dox-validate --format json FILE...                            # compare via the catalogue's veraPDF sources
.venv/bin/python -m pytest tests                                    # fixtures, suites and public documents
```
