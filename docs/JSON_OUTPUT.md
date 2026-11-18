# JSON output: `eqx-validator-output/1`

`dox-validate --format json` (short: `--json`) writes one JSON object per input file. This document describes
that object field by field. It is the contract for programs that consume the report; the text report
(`--format text`, the default) is for people and may change between versions.

- One file: one object followed by a newline. `--pretty` indents it.
- Several files: one object per line ([JSON Lines](https://jsonlines.org/)), in the order the files were given on
  the command line, whatever `--jobs` is. `--pretty` is ignored when there is more than one file.
- The report goes to stdout; diagnostics about the run go to stderr.
- The output is valid UTF-8. Invalid byte sequences in strings taken from the PDF become U+FFFD. Control
  characters, C1 controls and U+2028/U+2029 are written as `\u` escapes, so a JSON Lines record never contains a
  character that a line-based reader would treat as a line break.

A machine-readable JSON Schema of the format is in
[`schema/eqx-validator-output-1.schema.json`](../schema/eqx-validator-output-1.schema.json).

## Versioning

The `schema` field names the format and its major version: `"eqx-validator-output/1"`.

- Within `/1`, fields may be **added**, but existing fields are not removed, renamed or given a different type or
  meaning. **Consumers must ignore fields they do not know.**
- New values may appear in open sets: rule ids (the catalogue grows), `timing_ms` keys, `reason` texts and
  `diagnostics`. Closed sets (`summary.result`, the target `status` values, the combined-statement values,
  `severity`, `kind`) only change with a new major version.
- A change that would break a consumer that follows these rules gets a new major version
  (`eqx-validator-output/2`).
- `validator.version` is the tool version. `validator.catalog_sha256` identifies the rule catalogue the binary
  embeds. Two reports with the same catalogue hash were produced with the same rule definitions.

The JSON "EQX" prefix (also used by the rule ids) stands for EqualDoX.

## Top level

| Field | Type | Present | Meaning |
|---|---|---|---|
| `schema` | string | always | `"eqx-validator-output/1"` |
| `validator` | object | always | the tool that produced the report, see [validator](#validator) |
| `profiles` | array of strings | always | the 13 assessment targets, in this fixed order: `pdfua-1`, `pdfua-2`, `wtpdf-1.0-accessibility`, `wtpdf-1.0-reuse`, `wcag-2.0-A`, `wcag-2.0-AA`, `wcag-2.0-AAA`, `wcag-2.1-A`, `wcag-2.1-AA`, `wcag-2.1-AAA`, `wcag-2.2-A`, `wcag-2.2-AA`, `wcag-2.2-AAA` |
| `input` | object | always | the file, see [input](#input) |
| `rule_selection` | string | only with `--rules` | the rule id prefix that was evaluated, see [Rule selection](#rule-selection---rules) |
| `processed` | boolean | always | `false`: the file could not be validated at all (not found, not a PDF, encrypted with a user password, too many pages) |
| `file_repaired` | boolean | only if `processed` | `true`: qpdf had to reconstruct or replace part of the file, so the rules saw a repaired version. Every rule is then marked incomplete, so no target can reach `automated_pass_manual_required` |
| `error` | string | only if not `processed` | why the file was not processed |
| `summary` | object | always | overall result and counts. Its `exit_code` matches the process exit status for this file, see [summary](#summary-and-exit-status) |
| `findings` | array of objects | always | one record per rule that found at least one violation, see [findings](#findings) |
| `rules_evaluated` | array of strings | always | ids of the rules that ran to completion on this document (empty if not processed) |
| `rules_incomplete` | array of objects | always | rules that could not be evaluated, each `{"rule": id, "reason": text}` |
| `rules_not_applicable` | array of objects | always | rules outside their defined scope on this document, each `{"rule": id, "reason": text}` |
| `assessments` | object | always | 13 target assessments and 18 combined statements, see [assessments](#assessments) |
| `diagnostics` | array of strings | always | technical notes on the run: repairs qpdf made to the file, parts that could not be read completely, failures inside a rule module |
| `timing_ms` | object | always | wall-clock time of the validation steps in milliseconds, see [timing_ms](#timing_ms) |

### `validator`

| Field | Type | Meaning |
|---|---|---|
| `name` | string | `"dox-validate"` |
| `version` | string | tool version, as printed by `--version` |
| `catalog_sha256` | string | SHA-256 (hex) of the rule catalogue (`catalog/rules.json`) embedded at build time |
| `rule_semantics` | string | the reference tools and versions whose rule semantics the catalogue reproduces. veraPDF and PAC are **not** run by `dox-validate` |

### `input`

| Field | Type | Present | Meaning |
|---|---|---|---|
| `path` | string | always | the path as given on the command line |
| `sha256` | string | always | SHA-256 (hex) of the file content. Empty string if the file could not be read |
| `bytes` | integer | always | file size in bytes (`0` when the file cannot be read) |
| `pdf_version` | string | if `processed` | version from the file header, e.g. `"1.7"`, `"2.0"` |
| `pages` | integer | if `processed` | number of pages |
| `tagged` | boolean | if `processed` | the document has a structure tree (`/StructTreeRoot`) |
| `pdfua_part` | integer or null | if `processed` | the PDF/UA part the document declares (`pdfuaid:part` in the catalog's XMP metadata), `null` if it declares none |

## Summary and exit status

`summary` gives the overall result of one file. `summary.exit_code` is the exit status that file alone gives.
With several files the process exits with the highest of these values. Exit status 3 (usage error) and
4 (report could not be written) belong to the run, not to a file, so they never appear in a report.

### Result basis (`--target`)

The result is decided on one of three bases, selected with `--target` and reported in `summary.basis`.

| `basis` | Selected by | Deciding targets |
|---|---|---|
| `auto` | default | the PDF/UA part the document declares (`input.pdfua_part` 2: `pdfua-2`, otherwise `pdfua-1`), and `wcag-2.1-AA`, the WCAG level EN 301 549 V3.2.1 references |
| `targets` | `--target` with target names, e.g. `--target pdfua-2,wcag-2.2-AA` | the listed targets |
| `any` | `--target any`; the default with `--rules` | none: every error-severity finding counts, whatever target it concerns |

`--target` never changes `findings` or `assessments`. All 13 targets are always evaluated and reported.

On the `auto` and `targets` bases:

| `result` | `exit_code` | Condition |
|---|---|---|
| `not_processed` | 2 | the file could not be opened or read (`processed: false`) |
| `incomplete` | 2 | a deciding target could not be fully evaluated: its `unevaluated_rules` is not empty |
| `errors_found` | 1 | every deciding target fully evaluated, and at least one has status `fail` |
| `no_errors_found` | 0 | every deciding target fully evaluated, none fails. Findings that concern only other targets, warnings and error-severity findings that map to no target do not change this. **This is not a conformance statement**: manual evaluation is still required |

On the `any` basis:

| `result` | `exit_code` | Condition |
|---|---|---|
| `not_processed` | 2 | the file could not be opened or read |
| `incomplete` | 2 | at least one **required** rule could not be evaluated. A rule is required if the catalogue classes it as automatically checkable; all 2009 rules of the current catalogue are. With `--rules`, only required rules inside the selection count |
| `errors_found` | 1 | every required rule evaluated, and at least one finding has severity `error` |
| `no_errors_found` | 0 | every required rule evaluated, and no finding has severity `error` |

On every basis, `incomplete` takes precedence over `errors_found`. A deciding target that fails *and* has
unevaluated rules gives `incomplete` / exit 2, and is listed in both `failed_targets` and `incomplete_targets`.

Six PAC Quality recommendations (`EQX-PACQ-Fail*`) have severity `error`, but no standard requires them, so they
fail no target. On the `auto` and `targets` bases they do not affect the result. On the `any` basis, one of them
alone gives `errors_found` / exit 1.

| Field | Type | Meaning |
|---|---|---|
| `result` | string | `not_processed`, `incomplete`, `errors_found` or `no_errors_found` |
| `exit_code` | integer | 0, 1 or 2, as in the tables |
| `basis` | string | `auto`, `targets` or `any` |
| `targets` | array of strings | the deciding targets (empty for `any`) |
| `failed_targets` | array of strings | deciding targets with status `fail` (empty for `any`) |
| `incomplete_targets` | array of strings | deciding targets that could not be fully evaluated. If the file was not processed, all deciding targets (empty for `any`) |
| `error_rules` | integer | number of findings (rules) with severity `error`, over all targets |
| `warning_rules` | integer | number of findings (rules) with severity `warning`, over all targets |
| `error_occurrences` | integer | sum of `occurrences` over the error findings |
| `warning_occurrences` | integer | sum of `occurrences` over the warning findings |
| `rules_evaluated` | integer | number of entries in `rules_evaluated` |
| `rules_incomplete` | integer | number of entries in `rules_incomplete` |
| `rules_not_applicable` | integer | number of entries in `rules_not_applicable` |

The text report states the basis in its result line, e.g.
`Result (pdfua-1, wcag-2.1-AA): ERRORS FOUND - failing: pdfua-1, wcag-2.1-AA`, and always lists the deciding
targets among the assessments.

### Error, warning, incomplete, not processed

These terms mean different things and are kept apart throughout the report:

- **Error finding**: a rule with `severity: "error"` detected a violation. If the rule has a normative mapping to a
  target, that target's status is `fail`. A target the rule is mapped to with another obligation lists the finding
  in its `warning_rules` and does not fail because of it: the PAC document-language rules fail the WCAG targets and
  are only listed for `pdfua-1` (see [STANDARDS.md](STANDARDS.md#sources-of-the-rules)).
- **Warning finding**: `severity: "warning"`, either a possible problem that needs a human to look at it, or a
  recommendation. Warnings never fail a target. They are listed in the target's `warning_rules`.
- **Incomplete rule**: the rule could not be decided on this document, for example because a content stream
  could not be decoded, a font program was unreadable, the operator budget was reached, or a rule module failed.
  The rule appears in `rules_incomplete` with a `reason`, and every target that needs it lists it in
  `unevaluated_rules` and is `incomplete`, unless the target already fails. Findings the rule did report are
  kept.
- **Not processed**: the document could not be validated at all. `processed` is `false`, `error` says why,
  `findings`, `rules_evaluated` and `rules_incomplete` are empty, and every target is `incomplete` with all of
  its required rules in `unevaluated_rules`.

Nothing that was not checked is ever reported as passed.

## `findings`

One object per rule that found at least one violation, in catalogue order (sorted by rule id). A rule appears at
most once, whatever the number of occurrences.

| Field | Type | Meaning |
|---|---|---|
| `rule` | string | rule id, e.g. `EQX-UA1-7.21.4.1-1` (see [STANDARDS.md](STANDARDS.md) for the id families) |
| `severity` | string | `error` or `warning`. Where a rule stands for several equivalent reference checks, the highest of their severities |
| `kind` | string | `violation`, `warning` (needs review) or `recommendation` (PAC Quality) |
| `title` | string | the requirement, usually quoted from the reference rule |
| `explanation` | string | short description of what the check looks at |
| `module` | string | the rule module that evaluated it (`document`, `content`, `fonts`, `containment`, `structure`, `tables`, `pac_structure`, `pac_quality`, `annotations`) |
| `occurrences` | integer | number of times the violation was found. Always counted completely |
| `non_pac_occurrences` | integer | the occurrences found through a rule's veraPDF or EqualDoX definition, as opposed to through a PAC condition merged into the same rule (whose `detail` starts with `PAC:`) |
| `locations` | array of objects | distinct places of the violation, at most `--max-locations` (default 1000) per rule |
| `references` | object | normative references and the reference checks this rule stands for |

A **location** has:

| Field | Type | Meaning |
|---|---|---|
| `page` | integer or null | 1-based page number, `null` for objects that are not on a page (structure elements, metadata, the catalog) |
| `obj` | string or null | the innermost PDF object concerned, as `"<object number> <generation>"`, e.g. `"12 0"`. `null` if there is none (e.g. the file header) |
| `detail` | string | optional. Specifics that help to find and fix the problem (element type, MCID, font name, character code). A detail starting with `PAC:` means the violation was found through the PAC definition of a merged rule |

`references` has two arrays:

- `standards`: one entry per standard the rule is mapped to.
  - `standard`: `pdfua-1`, `pdfua-2`, `wtpdf-1.0-accessibility`, `wtpdf-1.0-reuse`, `wcag` or `pac-quality`.
  - `obligation`: `normative` (a violation fails the target), `warning` (listed only) or `recommendation`.
  - Optional fields: `clause` (clause of the standard), `sc` (WCAG success criterion), `level_by_version` (WCAG
    level of that criterion in 2.0 / 2.1 / 2.2, `null` for a version that does not contain it, e.g. 4.1.1 in
    2.2), `matterhorn`
    (Matterhorn Protocol failure conditions, a secondary reference), `via` (for ISO/TS 32005 rules: through which
    document the requirement applies).
- `sources`: the reference checks the rule reproduces.
  - veraPDF: `tool`, `version`, `profile` (`ua1`, `ua2`, `wt1a`, `wt1r`), `rule`, `specification`.
  - PAC: `tool`, `version`, `tab` (`PDF/UA`, `WCAG`, `Quality`), `check`, `condition`, `text`.
  - EqualDoX's own checks: `tool` (`"EqualDoX"`), `version`, `check`, `text`.

  Fields other than `tool` and `version` may be `null`.

## `rules_evaluated`, `rules_incomplete`, `rules_not_applicable`

- `rules_evaluated`: every rule that ran to completion, in catalogue order. A rule that found nothing and is in
  this list was checked and found no violation on this document.
- `rules_incomplete`: `{"rule", "reason"}`. Typical reasons: `"module <name> failed on this document"`,
  `"content streams could not be interpreted"`, `"content interpretation stopped at the operator budget"`,
  `"a content stream could not be decoded"`, `"the structure tree could not be read completely"`,
  `"qpdf repaired the file structure, the rules saw a reconstruction"`, or a rule-specific reason. A required rule
  that no module evaluates is listed with `"not implemented"`. This does not happen in a correct build, and
  `--self-check` verifies it.
- `rules_not_applicable`: rules that were evaluated but lie outside their defined scope. Currently these are the
  PAC-derived rules (`EQX-PAC-*`, `EQX-PACQ-*`) on PDF 2.0 documents: PAC 2026 defines its checks for PDF up to
  1.7. Their findings are removed and they are listed here with that reason. They are still counted in
  `rules_evaluated`. Merged rules apply only their veraPDF definition on such documents.

## `assessments`

### Single targets

One object for each of the 13 targets in `profiles`:

| Field | Type | Meaning |
|---|---|---|
| `status` | string | `fail`, `incomplete` or `automated_pass_manual_required` |
| `failed_rules` | array of strings | error-severity findings of rules with a normative mapping to this target |
| `warning_rules` | array of strings | other findings of rules mapped (normatively or not) to this target: warnings, review hints, and error findings whose mapping to this target is not normative |
| `unevaluated_rules` | array of strings | required rules of this target that were not evaluated or are incomplete |
| `manual_required` | array of strings | requirements of this target that need human evaluation. Fixed per target, taken from `catalog/matrix.json` |

Status rules, applied in this order:

1. `fail`: `failed_rules` is not empty.
2. `incomplete`: a required rule of the target was not evaluated (`unevaluated_rules` is not empty), or the file
   was not processed.
3. `automated_pass_manual_required`: otherwise. The automated checks found no failure; the requirements in
   `manual_required` still have to be evaluated by a person.

There is no `pass` status. `manual_required` ids are Matterhorn Protocol failure conditions (`MH-xx-yyy`) and PAC
checks (`PAC-…`) for PDF/UA-1, requirement ids `UA2-…` for PDF/UA-2, one `wtpdf-1.0-…-human` entry for each WTPDF
level, and success criteria `WCAG-x.y.z` for WCAG. How the targets are derived from the catalogue is described in
[STANDARDS.md](STANDARDS.md#assessment-model).

### Combined statements

Eighteen keys `<pdfua>+<wcag>`, one for `pdfua-1` and one for `pdfua-2`, each combined with each of the nine
WCAG targets (e.g. `pdfua-1+wcag-2.1-AA`). Each is `{"status", "meaning"}`:

| `status` | PDF/UA target | WCAG target | `meaning` |
|---|---|---|---|
| `both` | automated_pass_manual_required | automated_pass_manual_required | no automated failure for either target; manual evaluation is still required for both; not a conformance claim |
| `pdfua_only` | automated_pass_manual_required | fail | the WCAG target fails; the PDF/UA target has no automated failure; manual evaluation required |
| `wcag_only` | fail | automated_pass_manual_required | the PDF/UA target fails; the WCAG target has no automated failure; manual evaluation required |
| `neither` | fail | fail | both targets fail |
| `pdfua_fail_wcag_incomplete` | fail | incomplete | the PDF/UA target fails; the WCAG target could not be fully evaluated |
| `wcag_fail_pdfua_incomplete` | incomplete | fail | the WCAG target fails; the PDF/UA target could not be fully evaluated |
| `undetermined` | any other combination | | at least one target could not be fully evaluated |

## `timing_ms`

Wall-clock milliseconds (floating point) for one validation. Use these numbers to profile a run, not as a
contract. The set of keys may change.

| Key | Meaning |
|---|---|
| `hash` | SHA-256 of the input |
| `parse` | from the start of the validation until the document is open (includes `hash`) |
| `structure` | building the structure tree |
| `interpret` | interpreting all content streams once |
| `<module>` | running one rule module (`document`, `content`, `fonts`, …) |
| `<module>.prepare`, `create_modules` | setup steps, listed only when they take more than 1 ms |
| `assess` | deriving the assessments |
| `total` | the whole validation of this file |

## Rule selection (`--rules`)

`--rules PREFIX` evaluates only the rules whose id starts with `PREFIX` (for example `EQX-WCAG-`) and runs only the
modules that own them. The report then:

- has `rule_selection: PREFIX`;
- lists only selected rules in `findings`, `rules_evaluated` and `rules_incomplete`;
- leaves every rule outside the selection unevaluated, so targets that require such rules are `incomplete` and list
  them in `unevaluated_rules`;
- uses the `any` result basis unless `--target` is given, so `summary` and the exit status depend only on the
  selected rules. With an explicit `--target`, a deciding target that needs unselected rules is `incomplete`
  (exit 2).

A selection is a partial check. It never produces `automated_pass_manual_required` for a target that needs rules
outside the selection.

## Compatibility name `eqx-validate`

The build tree also contains a link `eqx-validate` to `dox-validate`, for existing EqualDoX tooling. Invoked under
that name, the program keeps the earlier defaults: the result basis is `any` unless `--target` is given, and
`--version` prints `eqx-validator VERSION`. The report itself is the same. `validator.name` is `"dox-validate"`
under both names.

## Example

The command below was run on a minimal hand-written one-page tagged PDF (an `H1` with text in non-embedded
Helvetica, no XMP metadata):

```console
$ dox-validate --format json --pretty report.pdf
```

The real output is 84 KB, mostly long id lists. To keep the example short, it has been cut down as follows.
`findings` shows 1 of 8 records. `assessments` shows 2 of 13 targets and 1 of 18 combined statements. Arrays
ending in a `"... N more"` string have been shortened; real reports never contain such a string. Everything else
is unchanged.

```json
{
  "schema": "eqx-validator-output/1",
  "validator": {
    "name": "dox-validate",
    "version": "0.2.0",
    "catalog_sha256": "6938a04122ea01c1ba44be1e894cbfcdbb34ad8202664d281ed9f348a02bbdf0",
    "rule_semantics": "veraPDF 1.30.2 (ua1, ua2, wt1a, wt1r) and PAC 26.1.0.0 (PDF/UA, WCAG, Quality); reference tools are not run"
  },
  "profiles": [
    "pdfua-1", "pdfua-2", "wtpdf-1.0-accessibility", "wtpdf-1.0-reuse",
    "wcag-2.0-A", "wcag-2.0-AA", "wcag-2.0-AAA",
    "wcag-2.1-A", "wcag-2.1-AA", "wcag-2.1-AAA",
    "wcag-2.2-A", "wcag-2.2-AA", "wcag-2.2-AAA"
  ],
  "input": {
    "path": "report.pdf",
    "sha256": "5b165c19cf9c46411f2754433a4c4e9fe17a79f4e48afaea6dc83e65cd717b38",
    "bytes": 1083,
    "pdf_version": "1.7",
    "pages": 1,
    "tagged": true,
    "pdfua_part": null
  },
  "processed": true,
  "file_repaired": false,
  "summary": {
    "result": "errors_found",
    "exit_code": 1,
    "basis": "auto",
    "targets": ["pdfua-1", "wcag-2.1-AA"],
    "failed_targets": ["pdfua-1", "wcag-2.1-AA"],
    "incomplete_targets": [],
    "error_rules": 8,
    "warning_rules": 0,
    "error_occurrences": 8,
    "warning_occurrences": 0,
    "rules_evaluated": 2009,
    "rules_incomplete": 0,
    "rules_not_applicable": 0
  },
  "findings": [
    {
      "rule": "EQX-UA1-7.21.4.1-1",
      "severity": "error",
      "kind": "violation",
      "title": "Fonts used for rendering must be embedded",
      "explanation": "a non-Type3/non-Type0 font used for rendering (Tr != 3) has no font program",
      "module": "fonts",
      "occurrences": 1,
      "non_pac_occurrences": 1,
      "locations": [
        { "page": 1, "obj": "4 0", "detail": "font Helvetica is not embedded" }
      ],
      "references": {
        "standards": [
          { "standard": "pdfua-1", "obligation": "normative", "clause": "7.21.4.1", "matterhorn": ["31-009"] },
          { "standard": "wcag", "obligation": "normative", "sc": "1.3.1",
            "level_by_version": { "2.0": "A", "2.1": "A", "2.2": "A" } }
        ],
        "sources": [
          { "tool": "veraPDF", "version": "1.30.2", "profile": "ua1", "rule": "7.21.4.1-1",
            "specification": "ISO 14289-1:2014" },
          { "tool": "PAC", "version": "26.1.0.0", "tab": "PDF/UA", "check": "FontsAreEmbedded",
            "condition": "CheckFailFontsAreEmbedded", "text": null }
        ]
      }
    }
  ],
  "rules_evaluated": [
    "EQX-PAC-FailActualTextHasLanguage",
    "EQX-PAC-FailAltTextHasLanguage",
    "EQX-PAC-FailAnnotTagInvalidSyntax",
    "... 2005 more"
  ],
  "rules_incomplete": [],
  "rules_not_applicable": [],
  "assessments": {
    "pdfua-1": {
      "status": "fail",
      "failed_rules": ["EQX-UA1-5-1", "EQX-UA1-7.1-8", "EQX-UA1-7.1-9", "EQX-UA1-7.21.4.1-1"],
      "warning_rules": [],
      "unevaluated_rules": [],
      "manual_required": ["MH-01-001", "MH-01-002", "MH-01-006", "... 51 more"]
    },
    "wcag-2.1-AA": {
      "status": "fail",
      "failed_rules": ["EQX-UA1-7.1-8", "EQX-UA1-7.1-9", "EQX-UA1-7.21.4.1-1"],
      "warning_rules": [],
      "unevaluated_rules": [],
      "manual_required": ["WCAG-1.1.1", "WCAG-1.2.1", "WCAG-1.2.2", "... 47 more"]
    },
    "pdfua-1+wcag-2.1-AA": {
      "status": "neither",
      "meaning": "both targets fail"
    }
  },
  "diagnostics": [],
  "timing_ms": {
    "hash": 0.457146, "parse": 0.562283, "structure": 0.023944, "interpret": 0.196552,
    "document": 0.037994, "content": 0.013082, "fonts": 0.002601, "containment": 0.690628,
    "tables": 0.004226, "pac_structure": 0.019878, "pac_quality": 0.02769, "annotations": 0.010552,
    "assess": 0.058722, "total": 2.05448
  }
}
```

A second hand-written file declares PDF/UA-1 (`pdfuaid:part` 1), embeds its font as a Type 3 font and has XMP
metadata. Its PDF/UA-2 and WTPDF targets fail (for example, `pdfuaid:part` is not 2 and the WTPDF declarations
are missing), but the deciding targets of the `auto` basis do not:

```console
$ dox-validate --json hello.pdf | jq '{input, summary}'
```

```json
{
  "input": {
    "path": "hello.pdf",
    "sha256": "71cc4dfb85bee562dab77614ddf8bb699e624ee890962b7535c74a1534ccc9f3",
    "bytes": 2441,
    "pdf_version": "1.7",
    "pages": 1,
    "tagged": true,
    "pdfua_part": 1
  },
  "summary": {
    "result": "no_errors_found",
    "exit_code": 0,
    "basis": "auto",
    "targets": ["pdfua-1", "wcag-2.1-AA"],
    "failed_targets": [],
    "incomplete_targets": [],
    "error_rules": 5,
    "warning_rules": 1,
    "error_occurrences": 5,
    "warning_occurrences": 1,
    "rules_evaluated": 2009,
    "rules_incomplete": 0,
    "rules_not_applicable": 0
  }
}
```

With `--target any`, the same file gives `errors_found` / exit 1 because of its five error findings.

A file that cannot be processed. The output is abbreviated in the same way, and only one target and one combined
statement are shown. All 13 targets are `incomplete`, and all 18 combined statements are `undetermined`.

```console
$ dox-validate --json missing.pdf
dox-validate: missing.pdf: not processed: file not found or not readable
```

```json
{
  "schema": "eqx-validator-output/1",
  "validator": {
    "name": "dox-validate",
    "version": "0.2.0",
    "catalog_sha256": "6938a04122ea01c1ba44be1e894cbfcdbb34ad8202664d281ed9f348a02bbdf0",
    "rule_semantics": "veraPDF 1.30.2 (ua1, ua2, wt1a, wt1r) and PAC 26.1.0.0 (PDF/UA, WCAG, Quality); reference tools are not run"
  },
  "profiles": ["pdfua-1", "pdfua-2", "... 11 more"],
  "input": { "path": "missing.pdf", "sha256": "", "bytes": 0 },
  "processed": false,
  "error": "file not found or not readable",
  "summary": {
    "result": "not_processed", "exit_code": 2,
    "basis": "auto", "targets": ["pdfua-1", "wcag-2.1-AA"], "failed_targets": [],
    "incomplete_targets": ["pdfua-1", "wcag-2.1-AA"],
    "error_rules": 0, "warning_rules": 0, "error_occurrences": 0, "warning_occurrences": 0,
    "rules_evaluated": 0, "rules_incomplete": 0, "rules_not_applicable": 0
  },
  "findings": [],
  "rules_evaluated": [],
  "rules_incomplete": [],
  "rules_not_applicable": [],
  "assessments": {
    "pdfua-1": {
      "status": "incomplete",
      "failed_rules": [],
      "warning_rules": [],
      "unevaluated_rules": ["EQX-PAC-FailActualTextHasLanguage", "... 181 more"],
      "manual_required": ["MH-01-001", "... 53 more"]
    },
    "pdfua-1+wcag-2.1-AA": {
      "status": "undetermined",
      "meaning": "at least one target could not be fully evaluated"
    }
  },
  "diagnostics": [],
  "timing_ms": { "hash": 0.011139, "parse": 0.020481, "structure": 0, "interpret": 0, "assess": 0, "total": 0.097373 }
}
```
