# dox-validate tests

Two layers:

- `tests/unit/` - C++ unit tests (`ninja -C build eqx-unit-tests && build/eqx-unit-tests`).
- `tests/*.py` - black-box tests of the `dox-validate` binary with an independent corpus (this document).

## Quick start

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build
python3 -m venv .venv && .venv/bin/pip install -r tests/requirements.txt
.venv/bin/python -m pytest tests                     # offline, about 15 s
DOX_VALIDATE=/path/to/dox-validate .venv/bin/python -m pytest tests   # another binary
```

The default run needs neither network nor generated files. Tests that use the on-demand size tiers or the downloaded
public documents skip until those exist:

```sh
python3 tests/corpus/generate.py --large     # medium/large tiers into tests/corpus/.cache/generated/ (~27 MB, seconds)
python3 tests/corpus/fetch_public.py         # core public set (~100 MB) into tests/corpus/.cache/public/
python3 tests/corpus/fetch_public.py --all   # plus the complete veraPDF PDF/UA-1 and PDF/UA-2 suites (~36 MB more)
```

## What is tested

| file | checks |
|---|---|
| `test_cli.py` | `--help`, `--version`, `--list-rules`, `--self-check`; usage errors exit 3 with nothing on stdout; stdout holds only the report, diagnostics go to stderr; JSON Lines for several files; exit status = maximum over files |
| `test_report.py` | every fixture report, under the default result basis and under `--target any`, validates against `schema/eqx-validator-output-1.schema.json` and the consistency rules in `report_invariants.py` (exit status, `summary.result`, deciding/failed/incomplete targets per basis, finding severities and counts, catalogue ids/severities/modules, page numbers within 1..pages, target statuses, combined statuses); the basis changes nothing but the result fields |
| `test_fixtures.py` | the construction-derived expectations of every fixture (`tests/corpus/fixtures/expected.json`) and of the generated tiers, under both result bases |
| `test_target.py` | `--target`: `auto` follows the declared PDF/UA part (`input.pdfua_part`) plus wcag-2.1-AA, explicit target lists, `any` = every error-severity finding, invalid values exit 3, the legacy name `eqx-validate` (`--target any` default, `eqx-validator` version string) |
| `test_contrast_cases.py`, `test_structure_cases.py` | cases built on the fly with the generator (not part of the corpus): WCAG 1.4.3 thresholds (normal/large/bold text, backgrounds, invisible and artifact text), ColSpan/RowSpan, role-mapped headings, nested lists |
| `test_determinism.py` | repeated runs, `--jobs 1/4/16` and single vs. multi-file runs give identical reports (except `timing_ms`) |
| `test_robustness.py` | malformed, encrypted and edge fixtures plus deterministic mutations (truncation, byte flips, inserted garbage, dropped keywords, deep nesting, reference loops): exit 0/1/2 only, a valid report, < 30 s and < 1 GiB peak RSS each |
| `test_options.py` | `--max-pages`, `--rules`, `--max-locations`, `--max-memory-mb` |
| `test_public.py` | downloaded documents (skipped when absent): schema and invariants; veraPDF corpus files against the verdict their file name states |

Known validator defects are listed in `tests/corpus/known_defects.json`; the affected fixtures are strict expected
failures (they fail again once the defect is fixed, so the list stays current). `tests/corpus/CORRECTNESS.md` has the
analysis.

Result bases: `expected.json` gives `exit_code`/`result` for the default basis (`auto`: the declared PDF/UA part and
wcag-2.1-AA decide, `deciding_targets`) and `any_basis` for `--target any`. Both follow from the expected target
statuses of the construction.

## The corpus

`tests/corpus/` holds an independent corpus; no file comes from, or is derived from, another dataset.

- `generate.py` writes the synthetic fixtures (`fixtures/*.pdf`, committed) and `fixtures/expected.json`;
  `generate.py --check` regenerates them in a temporary directory and compares every byte. The generator is standard
  library Python (`pdfgen.py`: PDF writer, tagged-document builder, RC4 encryption) and uses no compression, so the
  output does not depend on the zlib build. Each fixture changes one thing against a PDF/UA-1-clean baseline; its
  expectations (`must_report`, `must_not_report`, `must_report_any`, `may_report`, `exclusive_prefixes`, exit status,
  result, target statuses) and the normative basis (ISO 14289-1/-2 clause, Matterhorn condition, WCAG success criterion)
  come from the construction, never from a tool. `known_deviations` lists rules a fixture triggers against the normative
  reading where the reference tool (veraPDF 1.30.2) behaves the same.
- `fonts/`: Corpus Sans, an ASCII subset of Liberation Sans 2.1.5 renamed as the SIL OFL 1.1 requires (`OFL.txt`);
  `make_subset.py` rebuilds it with fontTools.
- `public/manifest.json`: public documents with URL, sha256, license, size, and `independent_expectation` (what the
  publisher or test-suite author states). Real-world documents (US federal works, CC BY papers, W3C techniques examples,
  LibreOffice guides) and the veraPDF PDF/UA-1 and PDF/UA-2 test suites at a pinned commit (CC BY 4.0). Only the
  manifest is committed; `fetch_public.py` downloads and verifies.
- `build_corpus.py` materialises the full corpus (fixtures + generated tiers + core public set) into one directory
  with `corpus_manifest.json` (path, sha256, bytes, pages, size tier, complexity, origin, tagged). `CORPUS_READY` records
  the build command and the manifest sha256 of the frozen corpus used for benchmarking.
- `expectations.py` compares reports with `expected.json` from the command line:
  `python3 tests/corpus/expectations.py -v [ID...]`.
