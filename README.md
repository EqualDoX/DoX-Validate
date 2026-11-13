# DoX-Validate

**The EqualDoX PDF Validator**: a native command-line tool for automated PDF accessibility checks.

`dox-validate` checks a PDF against PDF/UA-1, PDF/UA-2, WTPDF 1.0 and WCAG 2.0/2.1/2.2 in a single pass. It
reports every machine-checkable violation with its page, object and rule, and it lists what a person still has to
evaluate. It is developed by [EqualDoX](https://equaldox.de).

```console
$ dox-validate report.pdf
report.pdf
  PDF 1.7, 1 page(s), tagged, 2.0 ms
  Result (pdfua-1, wcag-2.1-AA): ERRORS FOUND - failing: pdfua-1, wcag-2.1-AA (8 rule(s) with errors, 0 with warnings in all targets)
  Assessments (automated checks only; manual evaluation is always required):
    pdfua-1                    fail                             failed 4, warnings 0, unevaluated 0, manual 54
    pdfua-2                    fail                             failed 3, warnings 0, unevaluated 0, manual 45
    wtpdf-1.0-accessibility    fail                             failed 3, warnings 0, unevaluated 0, manual 1
    wcag-2.1-AA                fail                             failed 3, warnings 0, unevaluated 0, manual 50
    wcag-2.2-AA                fail                             failed 3, warnings 0, unevaluated 0, manual 55
  ...
```

## Features

- **2008 rules, one pass.** The rules reproduce the machine-checkable checks of veraPDF 1.30.2 (PDF/UA-1, PDF/UA-2
  with ISO/TS 32005, WTPDF 1.0) and of PAC 2026 (PDF/UA, WCAG and Quality checks). EqualDoX adds its own WCAG colour
  contrast checks. Neither veraPDF nor PAC is run or needed.
- **Parse once, check everything.** The file is parsed once, the structure tree is built once, and every content
  stream is interpreted once. All rule modules share these models.
- **13 independent assessments.** PDF/UA-1, PDF/UA-2, WTPDF accessibility and reuse, and WCAG 2.0/2.1/2.2 at
  levels A/AA/AAA are assessed separately, plus 18 combined PDF/UA + WCAG statements.
- **Honest results.** A target is `fail`, `incomplete` or `automated_pass_manual_required`, never "pass". Content
  that could not be read or decided makes a target `incomplete` rather than passing it. Each target lists the
  requirements that need human evaluation.
- **Actionable findings.** Each finding has a stable rule id, the standard clause or WCAG success criterion, the
  reference checks it stands for, and its locations (page, PDF object, detail).
- **Built for automation.** Versioned JSON output ([`eqx-validator-output/1`](docs/JSON_OUTPUT.md)), JSON Lines for
  batches, and meaningful exit codes.
- **Parallel and bounded.** Many files are validated in parallel (`--jobs`). Memory limits per file and limits on
  decoded streams and interpreter work keep pathological files in check.
- **Robust on damaged files.** Files that qpdf can repair are validated as reconstructed. The repair is reported,
  and such a file never reaches an automated pass.
- **Local.** One native binary that reads the file and writes a report. No network access, no external
  processes.

## Installation

`dox-validate` is built from source. Linux is the tested platform; builds have been tested on Arch Linux and
Debian 13. Other systems are untested.

**Requirements:** a C++20 compiler (GCC 14 or newer), CMake ≥ 3.20, Ninja (or Make), pkg-config, Python 3 (build
time only), and the development packages of **qpdf ≥ 12**, pugixml and zlib.

| Distribution | Command |
|---|---|
| Arch Linux | `sudo pacman -S --needed base-devel cmake ninja python qpdf pugixml zlib` |
| Debian 13+, Ubuntu 25.10+ | `sudo apt install build-essential cmake ninja-build pkg-config python3 libqpdf-dev libpugixml-dev zlib1g-dev` |
| Fedora 43+ | `sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config python3 qpdf-devel pugixml-devel zlib-devel` |

Older releases (Debian 12, Ubuntu 24.04, Fedora 42) ship qpdf 11. On those systems, build
[qpdf](https://github.com/qpdf/qpdf) 12 from source first.

```console
$ git clone https://github.com/EqualDoX/DoX-Validate.git
$ cd DoX-Validate
$ cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
$ ninja -C build
$ sudo cmake --install build          # installs dox-validate to /usr/local/bin
```

Use `cmake --install build --prefix ~/.local` to install without root, or run `build/dox-validate` directly.

## Usage

```console
$ dox-validate report.pdf                         # text report
$ dox-validate -v report.pdf                      # all 13 targets and every stored location
$ dox-validate --format json --pretty report.pdf  # full JSON report
$ dox-validate --json --jobs 8 *.pdf > out.jsonl  # many files in parallel, one JSON object per line
$ dox-validate --target pdfua-2,wcag-2.2-AA report.pdf   # let these targets decide the result
$ dox-validate --rules EQX-WCAG- report.pdf       # only the rules with this id prefix
$ dox-validate --list-rules                       # rule catalogue: id, severity, module, title
```

The text report lists the findings with their locations:

```console
$ dox-validate report.pdf
  ...
  Findings: 8 rule(s) failed (8 error, 0 warning)
    [error] EQX-PACQ-FailOutlineIsNotPresentDespiteHeadings  The document has headings but no bookmarks
        at obj 1 0: headings present, no document outline items
    [error] EQX-UA1-5-1  XMP metadata must contain the PDF/UA identification schema
        at obj 1 0: PAC: XMP metadata missing
  ...
    [error] EQX-UA1-7.21.4.1-1  Fonts used for rendering must be embedded
        at page 1 obj 4 0: font Helvetica is not embedded
  ...
```

**What decides the result.** Every target is always evaluated and reported. The overall result and the exit
status, however, are decided by a few *deciding targets*, chosen with `--target`:

- `auto` (the default): the PDF/UA part the document declares in its XMP metadata (`pdfuaid:part` 2 means
  `pdfua-2`; otherwise `pdfua-1`), plus `wcag-2.1-AA`, the WCAG level EN 301 549 V3.2.1 references.
- A comma-separated list of targets, e.g. `--target pdfua-2,wcag-2.2-AA`.
- `any`: every error-severity finding counts, whatever target it concerns. This is the default with `--rules`.

`--target` never changes findings or assessments. A document can only conform to one PDF/UA part, so under
`auto` the requirements of the other part do not decide the result. In this example, the document declares
PDF/UA-1 and the automated checks find no failure for its deciding targets, while the PDF/UA-2 and WTPDF targets
fail:

```console
$ dox-validate hello.pdf
hello.pdf
  PDF 1.7, 1 page(s), tagged, 2.2 ms
  Result (pdfua-1, wcag-2.1-AA): NO ERRORS FOUND - automated checks only; manual evaluation is still required (5 rule(s) with errors, 1 with warnings in all targets)
  Assessments (automated checks only; manual evaluation is always required):
    pdfua-1                    automated_pass_manual_required   failed 0, warnings 0, unevaluated 0, manual 54
    pdfua-2                    fail                             failed 3, warnings 0, unevaluated 0, manual 45
    wtpdf-1.0-accessibility    fail                             failed 2, warnings 0, unevaluated 0, manual 1
    wcag-2.1-AA                automated_pass_manual_required   failed 0, warnings 0, unevaluated 0, manual 50
    wcag-2.2-AA                automated_pass_manual_required   failed 0, warnings 0, unevaluated 0, manual 55
  ...
$ echo $?
0
```

**Exit status** (with several files, the highest value over all files):

| Code | Meaning |
|---:|---|
| 0 | every file processed; no deciding target fails, and each was fully evaluated (warnings never fail; manual evaluation is still required) |
| 1 | a deciding target fails (with `--target any`: any error-severity finding) |
| 2 | a file could not be processed, or a deciding target could not be fully evaluated (with `any`: a required rule) |
| 3 | usage error |
| 4 | the report could not be written |

Reports go to stdout and diagnostics to stderr. `dox-validate --help` lists all options, including the limits
`--max-memory-mb`, `--max-pages` and `--max-locations`.

## JSON output

`--format json` writes one self-describing object per file. The excerpt below shows one of 8 findings and two of
13 targets. Some fields are omitted, and values ending in `…` are shortened.

```json
{
  "schema": "eqx-validator-output/1",
  "input": { "path": "report.pdf", "sha256": "5b165c19…", "bytes": 1083, "pdf_version": "1.7", "pages": 1, "tagged": true, "pdfua_part": null },
  "processed": true,
  "summary": {
    "result": "errors_found", "exit_code": 1,
    "basis": "auto", "targets": ["pdfua-1", "wcag-2.1-AA"], "failed_targets": ["pdfua-1", "wcag-2.1-AA"], "incomplete_targets": [],
    "error_rules": 8, "warning_rules": 0, "rules_evaluated": 2008, "rules_incomplete": 0
  },
  "findings": [
    {
      "rule": "EQX-UA1-7.21.4.1-1",
      "severity": "error",
      "kind": "violation",
      "title": "Fonts used for rendering must be embedded",
      "module": "fonts",
      "occurrences": 1,
      "locations": [ { "page": 1, "obj": "4 0", "detail": "font Helvetica is not embedded" } ]
    }
  ],
  "assessments": {
    "pdfua-1":     { "status": "fail", "failed_rules": ["EQX-UA1-5-1", "EQX-UA1-7.1-8", "EQX-UA1-7.1-9", "EQX-UA1-7.21.4.1-1"], "manual_required": ["MH-01-001", "…"] },
    "wcag-2.1-AA": { "status": "fail", "failed_rules": ["EQX-UA1-7.1-8", "EQX-UA1-7.1-9", "EQX-UA1-7.21.4.1-1"], "manual_required": ["WCAG-1.1.1", "…"] },
    "pdfua-1+wcag-2.1-AA": { "status": "neither", "meaning": "both targets fail" }
  }
}
```

The output is easy to process with standard tools:

```console
$ dox-validate --json report.pdf | jq -r '.findings[] | "\(.severity)\t\(.rule)\t\(.occurrences)"'
error	EQX-PACQ-FailOutlineIsNotPresentDespiteHeadings	1
error	EQX-UA1-5-1	1
...
```

Every field, the versioning policy and the relation between `summary` and the exit status are described in
[docs/JSON_OUTPUT.md](docs/JSON_OUTPUT.md). A JSON Schema is in
[`schema/eqx-validator-output-1.schema.json`](schema/eqx-validator-output-1.schema.json).

## Standards coverage and its limits

| Target | Rule ids | Basis |
|---|---|---|
| PDF/UA-1 (ISO 14289-1) | `EQX-UA1-*`, `EQX-PAC-*` | veraPDF 1.30.2 `ua1` profile, PAC 2026 PDF/UA checks (Matterhorn Protocol) |
| PDF/UA-2 (ISO 14289-2) | `EQX-UA2-*`, `EQX-TS32005-*` | veraPDF 1.30.2 `ua2` profile, including ISO/TS 32005 structure rules |
| WTPDF 1.0 accessibility / reuse | `EQX-WTPDF-*`, `EQX-UA2-*`, `EQX-TS32005-*` | veraPDF 1.30.2 `wt1a` / `wt1r` profiles |
| WCAG 2.0 / 2.1 / 2.2, A / AA / AAA | rules mapped to success criteria, `EQX-WCAG-*` | PAC 2026 WCAG checks, the criteria mappings of the rules above, EqualDoX colour checks |

PAC-derived rules apply to PDF up to 1.7, as PAC defines them, and are reported as not applicable on PDF 2.0.
EN 301 549 V3.2.1 chapter 10 applies WCAG 2.1 A/AA to documents, so the `wcag-2.1-AA` target is the relevant one
for it.

**Automated validation cannot prove accessibility.** No WCAG success criterion can be fully decided by a machine,
and many PDF/UA requirements need human judgement. Examples are whether alternative text is meaningful, whether
the reading order is logical, and whether content is correctly marked as an artifact. A report without failures
therefore shows that the automated checks found no violation. It is not proof of conformance with PDF/UA, WCAG or
EN 301 549, nor of compliance with any law. Each report lists the requirements still to be evaluated by a person
(`manual_required`).

Per-target rule counts, the assessment model and what is not covered: [docs/STANDARDS.md](docs/STANDARDS.md).

## Performance

<!-- BENCHMARK-SUMMARY: filled in from docs/BENCHMARKS.md once measurements are verified -->

*Benchmark results will be published here once the measurements are verified.*

## Reproducing the benchmarks

The benchmark method, corpus, hardware and full results will be documented in `docs/BENCHMARKS.md`, with the
scripts in `bench/`, when the results are published.

## Testing

```console
$ ctest --test-dir build              # or: build/eqx-unit-tests
$ build/dox-validate --self-check     # every catalogue rule has exactly one evaluating module
```

The unit tests need no external tools. The test corpus and the end-to-end test suite are described in
[tests/README.md](tests/README.md).

## Documentation

| Document | Content |
|---|---|
| [docs/JSON_OUTPUT.md](docs/JSON_OUTPUT.md) | the `eqx-validator-output/1` format |
| [docs/STANDARDS.md](docs/STANDARDS.md) | coverage per standard, assessment model, limits of automated validation |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | pipeline, modules, memory and parallelism, developing rules |
| [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) | third-party data, code and libraries |
| [CITATION.cff](CITATION.cff) | citation metadata |

## How to cite

If you use DoX-Validate in a scientific publication, cite it as shown under [License](#license) or use "Cite this
repository" on GitHub, which reads [CITATION.cff](CITATION.cff).

## License

DoX-Validate is licensed under the **EqualDoX Attribution License 1.0** (SPDX:
`LicenseRef-EqualDoX-Attribution-1.0`). It is a custom license, not an OSI-approved open-source license, and it is
not compatible with the GPL.

- **Free use.** Private, internal, academic and commercial use are permitted free of charge and without prior
  permission. You may modify and redistribute DoX-Validate, integrate it into proprietary applications and use it
  in commercial services, including SaaS.
- **Attribution for public products and services.** If you distribute a product that contains DoX-Validate, or
  provide a publicly accessible service that uses it, you must provide a reasonably accessible attribution that
  contains `https://equaldox.de`, for example in the documentation, acknowledgements or legal notices. It need not
  appear on every page. Use a functional link where links are supported; in media without clickable links
  (print, command-line output, plain text) the written-out URL is sufficient. Naming EqualDoX without the URL is
  not enough. Example: `Uses DoX-Validate by EqualDoX: https://equaldox.de`.
- **Internal use needs no attribution.** This includes use within your own organisation and its affiliated
  companies, and work as a contractor for a single customer's internal operations.
- **Citation in scientific publications.** If published research results were obtained using DoX-Validate, the
  publication must cite it, identifying Matthis Ehrhardt as the author, DoX-Validate as the software, EqualDoX as
  the developing organisation and the repository. Any conventional citation style may be used, for example:

  > Ehrhardt, Matthis. (2026). *DoX-Validate: PDF Accessibility Validator*. EqualDoX.
  > https://github.com/EqualDoX/DoX-Validate

  Citation metadata is in [CITATION.cff](CITATION.cff) ("Cite this repository" on GitHub). Research without a
  publication requires no citation.
- **Third-party components keep their own licenses** and are not subject to the attribution and citation
  requirements: the font parsers in `src/fonts/` (translated from veraPDF) are under the MPL-2.0; smaller parts are
  under BSD-3-Clause, Apache-2.0 and CC BY 4.0, and some data and test files under the W3C Document License, CC0 1.0
  and the SIL OFL 1.1. Each such file or part is marked and listed in
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), which also says where each license text is (mostly in
  [LICENSES/](LICENSES)). Binary distributions must include `THIRD_PARTY_NOTICES.md` and `LICENSES/` (installed by
  `cmake --install`).

The full terms, including warranty, liability and governing law, are in [LICENSE](LICENSE). This summary does not
replace them.

## About EqualDoX

DoX-Validate is developed and maintained by [EqualDoX](https://equaldox.de). The prefix `EQX` in rule ids and in
the output schema name stands for EqualDoX.
