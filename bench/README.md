# Performance benchmark: dox-validate vs. veraPDF (and cpdf)

The scripts in this directory measure dox-validate against veraPDF, the main open-source competitor, and against
cpdf's PDF/UA-1 verifier as a second tool. A separate cold-start comparison covers two further PDF/UA-1 checkers
with engines of their own, VellumPdf.Cli and outloud. They run on the frozen test corpus in `tests/corpus/`. The
method, environment, results and caveats are in [`docs/BENCHMARKS.md`](../docs/BENCHMARKS.md). The raw measurements
are in `results/`.

Faster execution does not mean the validation is equivalent. The tools check different rule sets (see
docs/BENCHMARKS.md, "Scope"). Correctness and coverage are analysed separately.

## Files

| file | purpose |
|---|---|
| `setup_tools.sh` | Downloads and installs the pinned tools into `.tools/` (gitignored), verifying every download by SHA-256: Temurin JRE 21.0.13+9, veraPDF 1.30.4 and 1.30.2 (greenfield), cpdf 2.9.3, VellumPdf.Cli 2.3.3 (release binary), outloud 0.2.1 (from PyPI into its own virtual environment; pip resolves its dependencies). It also builds `rusage_exec` and two Release binaries: `build-bench/dox-validate`, the measured one, with qpdf and pugixml linked statically (`-DDOX_STATIC_DEPS=ON`; their pinned source archives are downloaded on the first run), and `build-bench-shared/dox-validate`, the default build with the distribution's shared libqpdf. |
| `rusage_exec.c` | Small helper that runs one tool process and reports its wall time, user/sys CPU time and peak RSS (from `wait4`). |
| `bench.py` | Measurement harness, Python standard library only. Blocks: `env startup cold batch parallel`. |
| `ab_dox.py` | A/B of two dox-validate binaries, one process per document and alternating order, on selected or all (`--all`) corpus documents. It compares exit codes and findings (rule ids and occurrence counts). |
| `analyze.py` | Turns `results/*.csv` into `results/summary.json`, `results/tables.md` and `results/chart.svg`. |
| `analyze_ua1_tools.py` | Tables of the PDF/UA-1 tool comparison from a cold block: writes `tables.md` into the results directory it is given. |
| `report_diff.py` | Whole-report comparison of a dox-validate binary against a recorded baseline. `record` stores the exit status and a SHA-256 of stdout and stderr per document and output mode (JSON, text, other result basis, rule selections, location cap, reference traversal, `--text-model`, `--dump`); `check` lists every document on which another binary differs and can print the difference. Only the time measurements are removed before hashing. This is the correctness gate for performance work: a finding that moves, loses a location or changes its detail counts as a difference. |
| `ab_quick.py` | Development A/B of two or more builds (and tool variants such as `vellum-ua1`): one process per document, interleaved, medians per size tier, pages/s, median and maximum peak RSS. |
| `extension/manifest.json`, `extension/fetch.py`, `extension/CORPUS_READY` | The benchmark extension: 61 public documents that fill the page-count intervals the frozen corpus covers sparsely, with URL, sha256, pages, publisher and licence each, plus 14 anchor documents of the frozen corpus. `fetch.py` downloads and verifies them into `extension/.cache/` (gitignored) and writes the `corpus_manifest.json` that `bench.py --corpus` reads; `CORPUS_READY` freezes the set. |
| `charts.py` | The README charts: validation time and peak memory by page count, and the summary, as light and dark SVG files in `docs/charts/`, with `chart-data.json` and `chart-tables.md` (every plotted value). Reads the stored results only; the same inputs give the same files. |
| `results/` | Raw measurements (CSV, one row per tool process), `env.json`, `corpus.csv`, `load.jsonl`, and the derived summary. `results/ua1-tools-2026-11-14/` holds the PDF/UA-1 tool comparison, `results/optimised-2026-11-14/` the run that compares the optimised engine with the engine before and with the other tools (start-up, cold, parallel and batch blocks). |

## Reproduce

Prerequisites: Linux x86_64, Python ≥ 3.10, curl, unzip, a C compiler, and the dox-validate build dependencies
(CMake, Ninja, a C++20 compiler, qpdf ≥ 12, pugixml, zlib).

```sh
# 1. Tools (about 120 MB download) and the benchmark build of dox-validate (from the checked-out commit)
bench/setup_tools.sh

# 2. The frozen corpus: builds tests/corpus/.cache/full/ (bench.py refuses to run unless tests/corpus/CORPUS_READY
#    contains the sha256 of tests/corpus/.cache/full/corpus_manifest.json, and checks every file's sha256)
python3 tests/corpus/build_corpus.py
cat tests/corpus/CORPUS_READY

# 3. Measurements (CPU set 0-7, RSS sampler on CPU 16; about 4 hours on the reference host:
#    cold 82 min, parallel 60 min, batch about 80 min)
python3 -I bench/bench.py env cold parallel batch startup --cpus 0-7 --sample-cpu 16

# 4. A/B of two dox-validate builds over every corpus file (here: the measured build vs. another build, e.g. the
#    published release built from a separate checkout into OTHER_BUILD)
python3 -I bench/ab_dox.py build-bench/dox-validate OTHER_BUILD/dox-validate --all --reps 5 \
    --label f37a695:5ae7da2 --out bench/results/ab_published.csv

# 5. Aggregation (writes results/summary.json, results/tables.md, results/chart.svg)
python3 -I bench/analyze.py

# 6. PDF/UA-1 tool comparison, cold block only (about 20 min on the reference host), in its own results directory
python3 -I bench/bench.py env cold --cpus 0-7 --sample-cpu 16 \
    --cold-variants dox,dox-ua1rules,vellum-ua1,outloud-conf --results bench/results/ua1-tools-NEW
python3 -I bench/analyze_ua1_tools.py bench/results/ua1-tools-NEW

# 7. Before/after of the optimised engine together with the other tools (about 2 hours on the reference host).
#    The engine before is a Release build of 729a5c5 in build-baseline (or the binary $DOX_BASELINE names).
git worktree add ../dox-validate-729a5c5 729a5c5
cmake -S ../dox-validate-729a5c5 -B build-baseline -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build-baseline dox-validate
R=bench/results/optimised-NEW
python3 -I bench/bench.py env startup cold --cpus 0-7 --sample-cpu 16 \
    --startup-variants dox,dox-shared,dox-base,vellum-ua1,outloud-conf \
    --cold-variants dox,dox-shared,dox-base,dox-ua1rules,vellum-ua1,outloud-conf,vp-ua1,cpdf-ua1 --results $R
python3 -I bench/bench.py parallel --cpus 0-7 --sample-cpu 16 \
    --parallel-variants dox,dox-base --pool-variants dox,dox-base,vellum-ua1 --results $R
python3 -I bench/bench.py batch --cpus 0-7 --sample-cpu 16 --reps 3 \
    --batch-variants dox,dox-base,vellum-ua1,outloud-conf --results $R
python3 -I bench/analyze_ua1_tools.py $R

# 8. The README charts. The extension documents (about 240 MB) and one cold block over them (about 2 hours on the
#    reference host; anchors and extension documents together), then the charts from both stored runs:
python3 -I bench/extension/fetch.py
python3 -I bench/bench.py env cold --cpus 0-7 --sample-cpu 16 --corpus bench/extension/.cache \
    --cold-variants dox,dox-shared,vellum-ua1,outloud-conf,vp-ua1,cpdf-ua1 --results bench/results/extension-NEW
python3 -I bench/charts.py      # reads the run directories named in its RUNS list; writes docs/charts/

# 9. Whole-report comparison of two builds: every document on which the reports differ in any of the output modes
python3 -I bench/report_diff.py record build-baseline/dox-validate --out base.jsonl \
    --modes json,text,any,dedup,ua1rules,wcagrules,maxloc,textmodel,dump tests/corpus/.cache/full
python3 -I bench/report_diff.py check build-bench/dox-validate --against base.jsonl \
    --baseline-bin build-baseline/dox-validate
```

Each block can be run on its own, for example `python3 -I bench/bench.py env cold`. A block overwrites its own CSV
file. `load.jsonl` is appended to. The options are listed by `python3 -I bench/bench.py --help`; the defaults are
the settings used for the published results (5 repetitions, 3 passes in the batch block, 1/2/4/8 workers).

`--allow-unfrozen --limit N --results DIR` exists only to test the scripts. Results produced with it are not valid.

## What is run

All tools write a machine-readable JSON report to a file on tmpfs. The harness parses it afterwards and checks
that every file was processed. Tool defaults are kept: no JVM options and no veraPDF option that limits
checks or failures; dox-validate always evaluates its whole catalogue.

| variant | command per run |
|---|---|
| `dox` | `build-bench/dox-validate --format json --jobs N FILE...` (the build with statically linked qpdf) |
| `dox-ua1rules` | same, plus `--rules EQX-UA1-` (only the 106 rules mirroring veraPDF's PDF/UA-1 profile; supplementary) |
| `dox-shared` | the same call with `build-bench-shared/dox-validate`, the default build with the distribution's shared libqpdf (`$DOX_SHARED` names another binary) |
| `dox-base`, `dox-base-ua1rules` | the same calls with a second dox-validate build for before/after comparisons: `$DOX_BASELINE`, default `build-baseline/dox-validate` |
| `vp-ua1`, `vp-ua2`, `vp-wt1a`, `vp-wt1r` | `.tools/verapdf-1.30.4/verapdf -f PROFILE --format json [--processes N] FILE...` with `JAVA_HOME=.tools/jre-21.0.13+9` |
| `vp1.30.2-ua1` | the same with veraPDF 1.30.2 |
| `cpdf-ua1` | `.tools/cpdf-2.9.3/cpdf -verify "PDF/UA-1(matterhorn)" -json FILE` (one file per process; cpdf has no batch mode) |
| `vellum-ua1` | `.tools/vellum-2.3.3/vellum-preflight -p ua1 -f json FILE...` |
| `outloud-conf` | `.tools/outloud-0.2.1/bin/outloud --layer conformance -q --json - FILE...` (its semantic layer is not run) |

Every tool process is pinned to the CPU set through the spawning thread's affinity. A file counts as processed if:

- dox-validate: a report with `processed: true`;
- veraPDF: a job with a `validationResult` whose `jobEndStatus` is `normal`;
- cpdf: exit status 0 and a JSON array;
- VellumPdf: a report entry with a boolean `conformant`;
- outloud: a result entry without `error`.

Anything else is recorded as a failure and excluded from the timing statistics.

Timeouts:

- One-document processes (startup, cold, pool): 120 s, about 10× the slowest veraPDF document.
- Whole-corpus processes (batch, native parallel): 1800 s per pass.

A process that reaches its timeout is killed and counted as a failure (`timeout` / `n_file_timeouts`).

If a tool crashes sporadically, re-run the affected block pinned to `--cpus 0-7` before counting it as a tool failure.
Some cores of the reference host are unstable.
