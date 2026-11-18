# Architecture

`dox-validate` is a C++20 command-line program. It parses untrusted PDFs, interprets every content stream, reads
embedded font programs and XMP metadata, and evaluates about 2000 rules. It was designed to start quickly, run with
bounded memory, scale across files in parallel, and build on a plain Linux system from distribution packages.

## Technology

| Part | Choice | Why |
|---|---|---|
| PDF object layer | [qpdf](https://qpdf.sourceforge.io/) ≥ 12 (C++ API) | full object model, all standard filters, RC4/AES encryption, cross-reference and object streams, repair of damaged files; widely packaged and fuzz-tested |
| Content streams | own lexer and interpreter | allocation-light; qpdf's tokenizer allocates a string per token |
| Fonts | own ports of veraPDF 1.30.2's font parsers (PostScript/Type 1, CFF, CID-keyed, TrueType, embedded and predefined CMaps) | veraPDF's parsers decide findings through details that general font libraries do not reproduce: which glyphs exist, fractional CFF widths, how a broken program counts |
| XMP | own strict XML reader | veraPDF rejects malformed XMP packets that lenient parsers accept; a lenient parser would turn those failures into passes |
| Annotation rich text (`/RC`) | [pugixml](https://pugixml.org/) | |
| Compression | [zlib](https://zlib.net/) | |
| Build-time code generation | Python 3 (standard library) | the rule catalogue is compiled into the binary |

Python is not needed at run time. The binary never runs veraPDF, PAC or any other external program.

### Shared or static dependencies

The default build links the distribution's shared qpdf and pugixml (`pkg-config`); nothing is downloaded.

`-DDOX_STATIC_DEPS=ON` builds qpdf 12.4.2 and pugixml 1.16 from their upstream release archives and links them,
libstdc++ and libgcc statically. The archives are fetched at configure time into `DOX_DEPS_DIR` (default
`<build>/deps`) and checked against the SHA-256 pinned in `CMakeLists.txt`; an archive that is already there with the
right hash is not fetched again, so an offline build can provide them. qpdf is built with its native crypto provider
only.

The reason is process start-up. A distribution `libqpdf.so` brings GnuTLS, OpenSSL and their dependencies with it:
25 shared objects and about 9,700 symbol relocations, roughly 2 ms in every process, more than validating a small
document takes. The static binary loads libc, libm, libz and libjpeg. Both builds produce the same reports; the
benchmark measures the static one and reports the default build next to it
([BENCHMARKS.md](BENCHMARKS.md)). With the pinned qpdf, `core/cos.cpp` also uses qpdf's own single-search dictionary
lookup, which the shared library does not export (`DOX_QPDF_PINNED`).

## Pipeline

Each file goes through these steps:

```
Document::open        qpdf instance, catalog, trailer, raw header, page list with inherited attributes
  -> StructTree::build  structure elements, kids (elements, MCIDs, MCRs, OBJRs), RoleMap, ParentTree,
                        standard types under PDF 1.7 and ISO 32000-2 semantics
  -> module prepare()   rule modules register ContentObservers (glyph and operator callbacks)
  -> interpret          every content stream once: pages, Form XObjects, annotation appearances,
                        tiling patterns, Type 3 glyphs -> ContentModel (stream instances, marked-content
                        frames, content items, text runs); glyph events go to the observers, not into memory
  -> module run()       each module reads the shared, read-only models and reports findings
  -> assess()           13 independent target assessments and 18 combined statements
  -> summarize()        overall result and exit status from the deciding targets (--target: auto, a list, any)
  -> report             eqx-validator-output/1 JSON or the text report
```

- **Parse once.** One qpdf instance, one structure tree and one interpretation of each content stream instance are
  shared by all modules. Each font dictionary is loaded once, and each font program is parsed once. The input file is
  opened once: one source serves the SHA-256, the header and trailer checks and qpdf.
- **Content one page at a time.** Content items and text runs are kept only until the page they belong to has been
  interpreted. A module that needs them takes what its rules read in an observer when the page's content stream ends
  (`on_stream_end`), and reports in `run()`, in the same order as if it had walked the whole document there. The
  interpreter then gives the page's items and runs back, so the content model does not grow with the document.
  Marked-content frames and stream instances stay for the whole validation. `--dump` and `--text-model` keep the
  whole model.
- **Modules are released after their run.** A module's tables do not stay alive while the later modules run.
- **Modules own disjoint rule sets.** Every catalogue rule has exactly one owning module
  (`src/rules/ownership.cpp`, which assigns rules by catalogue family). `--self-check` verifies this.
- **Findings** keep one record per rule. Each record stores every distinct location (page, innermost object,
  detail) up to `--max-locations`, while the occurrence count is always complete. The severity is taken from the
  catalogue.
- **Nothing passes silently.** A module that throws marks all of its rules incomplete. A rule that cannot be
  decided on a document is marked incomplete with a reason. A rule that no module evaluates is reported as
  "not implemented". Each of these makes the targets that need the rule `incomplete`, never passed.

| Module | Rules | Scope |
|---|---:|---|
| `document` | 46 | catalog, identification, metadata and XMP, syntax, viewer preferences, encryption, optional content, embedded files, document language, destinations |
| `content` | 14 | tagged vs. artifact content, page-content language, XObject usage, PAC text contrast, parsability |
| `fonts` | 33 | font embedding, glyph presence and widths, encodings, CMaps, ToUnicode, Private Use Area glyphs |
| `containment` | 1667 | parent/child rules of the structure types (PDF/UA-2, WTPDF, ISO/TS 32005), table-driven |
| `structure` | 29 | headings, role mapping, alternative descriptions, notes, TOC, lists, formulas, language of structure alternatives |
| `tables` | 15 | table regularity and header association |
| `pac_structure` | 134 | PAC 2026 structure-element checks |
| `pac_quality` | 28 | PAC 2026 quality and WCAG-tab checks, and the EqualDoX WCAG colour checks |
| `annotations` | 43 | annotations, forms, links, media, page tab order |

The rule counts are those printed by `dox-validate --self-check`.

## The rule catalogue

`catalog/` holds the data that defines the rules:

| File | Content |
|---|---|
| `rules.json` | every rule: id, title, kind, severity, automation class, standards mappings with obligation, the reference checks it stands for, detection condition, family |
| `wcag2.json` | the WCAG success criteria with their level in WCAG 2.0, 2.1 and 2.2 |
| `matrix.json` | per target, the requirements and their class (automatic / partial / manual / unsupported); the `manual_required` lists; the EN 301 549 clause 10 mapping |

At build time, `tools/gen_catalog.py` turns these files into `build/gen/catalog_data.cpp` and
`build/gen/catalog_index.h`. The header holds the rule ids and families as constant expressions, so `R("EQX-…")`
with a literal, the rule-ownership table and the table of the 1667 containment rules are resolved by the compiler
(an unknown rule id is a compile error) and cost nothing at start-up. For every rule the generator
computes two target bitmasks: the targets the rule can fail (normative mappings, with WCAG level and version
propagation as described in [STANDARDS.md](STANDARDS.md#assessment-model)) and the targets it concerns at all. It
also embeds each rule's references, already rendered as JSON, the `manual_required` list of every target, and the
SHA-256 of `rules.json`, which appears as `catalog_sha256` in every report. When any of the three files changes,
CMake regenerates the code. The CMake cache variable `DOX_CATALOG_DIR` points the build at another catalogue
directory.

## Parallelism and memory

- **Across files, not within a file.** `--jobs N` (default: number of CPUs) validates up to N files at once, each
  with its own qpdf instance on its own thread. A qpdf instance is not thread-safe, so one file is always
  processed by one thread. qpdf's process-wide lazy state is initialised once, before any validation starts.
- **Single-file runs stay single-threaded.** With one file, the validation runs on the main thread, switched to a
  large stack, so the process needs no atomic reference counts or malloc locks.
- **Reports are written as results complete.** In a run over several files, a result is written as soon as it and
  the results of all earlier files are done, in argument order, and released right after. The output is the same as
  if every report were written at the end.
- **The process exits without tearing down.** Once the reports are flushed and the exit status is known, the process
  ends with `_Exit`; in a one-file run the document, its models and qpdf's objects are not destroyed first. Sanitizer
  builds and builds with `-DDOX_FULL_TEARDOWN` return from `main` normally, so leak checks and coverage counters see
  everything released.
- **Stack.** Every validation runs on its own 256 MiB stack, whatever `RLIMIT_STACK` is set to. Content streams
  are interpreted on a heap-allocated stack, so nested forms have no depth limit. The structure tree is collected
  with an explicit stack, and cycles are detected and fail closed.
- **Streaming.** Glyph-level checks run as observers while the content is interpreted, and nothing is stored per
  glyph. Content items and text runs are compact records (56 and 16 bytes; attributes that repeat from run to run are
  stored once, text in an arena at 1, 2 or 4 bytes per code point) and live for one page (see the pipeline notes
  above). Reading an item or run of a page that was given back throws, so a missed case makes rules incomplete
  instead of reading stale data.
- **What bounds memory on large documents.** qpdf keeps every object it has parsed while the document is open, and
  the rules read the structure tree's dictionaries until the end. A structure element costs about 1 KB in qpdf's
  object model, so on heavily tagged documents qpdf's objects are the largest block (75 to 97 MB of a heap peak of
  104 to 158 MB on the three most structure-heavy corpus documents).
- **Limits.** Each limit marks the affected rules incomplete instead of letting them pass:
  - one decoded stream: 1 GiB (zlib and run-length decoders);
  - content interpretation: above 50 million operators or 1 million stream instances, a stream that was already
    read in the same context is not read again; at 200 million operators, interpretation stops;
  - address space: `--max-memory-mb` (default 4096 MiB per concurrently validated file) sets `RLIMIT_AS` for the
    process to that value times the number of worker threads, plus their stacks. It never raises a limit that is
    already lower, and it is not applied in sanitizer builds. A file that does not fit is reported as not
    processed or incomplete;
  - limits that qpdf itself enforces (container sizes, nesting, errors per object). These are detected with
    qpdf ≥ 12.3. The count is per process, so with `--jobs`, a limit hit in one file also marks files validated
    at the same time as incomplete, which errs on the safe side.
- **Damaged files.** If qpdf has to reconstruct the cross-reference table or replace objects, the rules run on the
  reconstruction. If qpdf meets damage only after the rules have started reading, the file is read again through
  a recovered view. Either way the repairs are listed in `diagnostics`, `file_repaired` is `true`, and every rule
  is marked incomplete.

## Result basis and compatibility name

The overall result and the exit status are computed after the assessments (`summarize` in `src/report/output.cpp`),
from the result basis given by `--target`:

- `auto`: the PDF/UA part declared in the catalog's XMP (`pdfuaid:part`, read by the `document` module and
  reported as `input.pdfua_part`), plus `wcag-2.1-AA`;
- an explicit list of targets;
- `any`: every error-severity finding.

The basis never changes findings or assessments. Details: [JSON_OUTPUT.md](JSON_OUTPUT.md#summary-and-exit-status).

The build tree contains a link `eqx-validate` to `dox-validate` (target `legacy-name-link`; it is not installed),
for existing EqualDoX tooling. Invoked under that name, the program keeps the earlier defaults: `--target any`, and
`--version` prints `eqx-validator VERSION`.

## Source layout

| Path | Content |
|---|---|
| `src/main.cpp` | command line (including `--target` parsing and the compatibility name), worker threads, stack and memory limits |
| `src/engine/` | `validate_file` (the pipeline above), `--dump` and `--text-model` views |
| `src/core/` | `Document` (qpdf wrapper), `SourceFile` (the input file, opened once), `cos` helpers, `StructTree`, `TreeShape` (decoded number and name trees), content lexer and interpreter (`ContentModel`, `ContentObserver`), damaged-file recovery |
| `src/fonts/` | font model and the ports of veraPDF's font parsers; `generated/` holds the glyph list, encodings, standard-14 metrics and predefined CMaps (produced by `tools/gen_font_data.py`) |
| `src/rules/` | catalogue access (`catalog.*`), findings collector, `ValidationContext` and `RuleModule` (`module.h`), ownership, module registry |
| `src/rules/<module>/` | one directory per rule module |
| `src/report/` | assessments (`assess.*`), JSON and text output (`output.*`) |
| `src/util/` | JSON writer, SHA-256, text helpers, "not read completely" bookkeeping |
| `catalog/` | the rule catalogue (see above) |
| `tools/` | code generators (`gen_catalog.py`, `gen_font_data.py`, `gen_lang_registry.py`) and `catalog_stats.py` |
| `tests/unit/` | unit tests (`unit.h`: `EQX_TEST`, `EXPECT`, `EXPECT_EQ`) |

## Developing rule modules

A module is a class derived from `RuleModule` (`src/rules/module.h`). It is listed in `src/rules/registry.cpp`
and owns the rules that `src/rules/ownership.cpp` assigns to it.

- `prepare(ctx, observers)` runs before content interpretation. Register a `ContentObserver` there if the module
  needs glyph or operator events.
- `run(ctx)` runs once the models are complete. It must evaluate **every** rule in `owned_rules(name())`.
- Content items and text runs (`ctx.content.items`, `ctx.content.texts`) exist only while their page is interpreted.
  A module that needs them registers an observer and, in `on_stream_end` of a page stream, copies what its rules
  read. That step reads the content model only: no PDF object, no `ctx.report`, no `mark_incomplete`. Reports and
  every read of a PDF object stay in `run()`, in item order, so findings, diagnostics and failures come out as if
  `run()` had walked the whole document. If taking a page throws, keep the exception and throw it again in `run()`.
  The content and pac_quality modules show the pattern. Frames, stream instances and the structure tree can be read
  in `run()` as before.
- Report a violation with `ctx.report(rule, page_index, obj, "detail")`. `rule` is the catalogue index, written
  `R("EQX-…")`. `page_index` is 0-based, or -1 when the object is not on a page. `obj` is the innermost object of
  the violation. The detail is free text that helps a user find and fix the problem.
- If a rule cannot be decided on a document, call `ctx.mark_incomplete(rule, "reason")`. Never skip a rule
  silently, and never report a violation you cannot substantiate.
- Severities come from the catalogue. Do not set them in code.
- Keep to one pass over the models: no quadratic walks over large trees, and no per-glyph allocations.

Useful commands:

```console
$ build/dox-validate --self-check                                  # every rule has exactly one owner
$ build/dox-validate --list-rules | awk -F'\t' '$3 == "fonts"'     # id, severity, module, title
$ build/dox-validate --rules EQX-WCAG- file.pdf                    # run only the modules owning these rules
$ build/dox-validate --dump file.pdf                               # parsed model: structure tree, streams, frames, items
$ build/dox-validate --text-model file.pdf                         # eqx-text-model/1: elements and text runs
$ build/eqx-unit-tests [name-substring]                            # unit tests
```

To change a rule's title, severity or standards mapping, edit `catalog/rules.json` (and `catalog/matrix.json` if
the requirement classes change) and rebuild. The generated code and `catalog_sha256` follow automatically. A new
rule also needs an owner in `ownership.cpp` and an implementation in that module. `--self-check` fails until both
exist.
