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
| `setup_tools.sh` | Downloads and installs the pinned tools into `.tools/` (gitignored), verifying every download by SHA-256: Temurin JRE 21.0.13+9, veraPDF 1.30.4 and 1.30.2 (greenfield), cpdf 2.9.3, VellumPdf.Cli 2.3.3 (release binary), outloud 0.2.1 (from PyPI into its own virtual environment; pip resolves its dependencies). It also builds `rusage_exec` and the Release binary `build-bench/dox-validate`. |
| `rusage_exec.c` | Small helper that runs one tool process and reports its wall time, user/sys CPU time and peak RSS (from `wait4`). |
| `bench.py` | Measurement harness, Python standard library only. Blocks: `env startup cold batch parallel`. |
| `ab_dox.py` | A/B of two dox-validate binaries, one process per document and alternating order, on selected or all (`--all`) corpus documents. It compares exit codes and findings (rule ids and occurrence counts). |
| `analyze.py` | Turns `results/*.csv` into `results/summary.json`, `results/tables.md` and `results/chart.svg`. |
| `analyze_ua1_tools.py` | Tables of the PDF/UA-1 tool comparison from a cold block: writes `tables.md` into the results directory it is given. |
| `results/` | Raw measurements (CSV, one row per tool process), `env.json`, `corpus.csv`, `load.jsonl`, and the derived summary. `results/ua1-tools-2026-11-14/` holds the PDF/UA-1 tool comparison. |

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
| `dox` | `build-bench/dox-validate --format json --jobs N FILE...` |
| `dox-ua1rules` | same, plus `--rules EQX-UA1-` (only the 106 rules mirroring veraPDF's PDF/UA-1 profile; supplementary) |
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
