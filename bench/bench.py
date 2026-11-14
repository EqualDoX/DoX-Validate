#!/usr/bin/env python3
"""Performance benchmark harness: dox-validate versus veraPDF on the frozen test corpus (Python standard library only).

Blocks (each writes CSV rows to bench/results/<block>.csv; see bench/README.md):
  env       environment record (CPU, kernel, compiler, tool versions, corpus hash)      -> env.json
  startup   process start-up: --version and the smallest corpus file, one process per call
  cold      one process per file and tool (cold start), every file, interleaved repetitions
  batch     one process per tool over the whole corpus, several passes in the same process (warm / sustained);
            per-file times reported by the tools themselves go to batch_files.csv
  parallel  corpus throughput with 1, 2, 4, 8 workers: the tools' own parallelism (dox-validate --jobs N,
            veraPDF --processes N) and a pool of N concurrent one-file processes

Measurement: each tool process is started through the small helper bench/rusage_exec.c (spawned from a thread pinned
to the benchmark CPU set, so the tool inherits the affinity); the helper measures wall time (CLOCK_MONOTONIC around
fork..wait4), CPU time and peak RSS from the wait4 rusage of the tool process (ru_maxrss of the largest process in the
tree; the JVM is the tool process itself because the veraPDF launcher execs java).  Process trees with several processes (veraPDF --processes, worker pools) additionally get
a sampled sum of the RSS of all live processes in the tree (20 ms interval, sampler pinned outside the CPU set).
Reports are written as JSON to a tmpfs file for both tools and checked per file afterwards (processed / failed).
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import random
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
TOOLS = HERE / ".tools"
RESULTS = HERE / "results"
JRE = TOOLS / "jre-21.0.13+9"
DOX = ROOT / "build-bench" / "dox-validate"
RUSAGE_EXEC = TOOLS / "rusage_exec"
CPDF = TOOLS / "cpdf-2.9.3" / "cpdf"
VERAPDF = {"1.30.4": TOOLS / "verapdf-1.30.4" / "verapdf", "1.30.2": TOOLS / "verapdf-1.30.2" / "verapdf"}
VELLUM = TOOLS / "vellum-2.3.3" / "vellum-preflight"
OUTLOUD = TOOLS / "outloud-0.2.1" / "bin" / "outloud"
CLK_TCK = os.sysconf("SC_CLK_TCK")
PAGE = os.sysconf("SC_PAGE_SIZE")

# ----------------------------------------------------------------------------------------------------------------- tools

# variant -> (tool family, argv builder). files: list of paths; workers: tool-internal parallelism.
def argv_for(variant: str, files: list[str], workers: int = 1) -> list[str]:
    if variant == "dox":            # full catalogue (all targets), the normal use
        return [str(DOX), "--format", "json", "--jobs", str(workers), *files]
    if variant == "dox-ua1rules":   # supplementary: only the rules that mirror veraPDF's PDF/UA-1 profile
        return [str(DOX), "--format", "json", "--jobs", str(workers), "--rules", "EQX-UA1-", *files]
    if variant == "cpdf-ua1":       # secondary competitor: cpdf's partial Matterhorn (PDF/UA-1) verifier, one file per run
        assert len(files) <= 1 and workers == 1
        return [str(CPDF), "-verify", "PDF/UA-1(matterhorn)", "-json", *files]
    if variant == "vellum-ua1":     # VellumPdf.Cli: its PDF/UA-1 profile (own engine); several files in one process
        assert workers == 1
        return [str(VELLUM), "-p", "ua1", "-f", "json", *files]
    if variant == "outloud-conf":   # outloud: its PDF/UA-1 conformance layer (the semantic layer is not a PDF/UA check)
        assert workers == 1
        return [str(OUTLOUD), "--layer", "conformance", "-q", "--json", "-", *files]
    m = re.fullmatch(r"vp(\d+\.\d+\.\d+)?-(ua1|ua2|wt1a|wt1r)", variant)
    if m:
        exe = VERAPDF[m.group(1) or "1.30.4"]
        extra = ["--processes", str(workers)] if workers > 1 else []
        return [str(exe), "-f", m.group(2), "--format", "json", *extra, *files]
    raise ValueError(variant)


def tool_env() -> dict[str, str]:
    env = {k: v for k, v in os.environ.items() if k not in ("JAVA_OPTS", "JAVA_TOOL_OPTIONS", "_JAVA_OPTIONS", "JDK_JAVA_OPTIONS")}
    env["JAVA_HOME"] = str(JRE)
    env["LC_ALL"] = "C.UTF-8"
    return env


# -------------------------------------------------------------------------------------------------------- report checks

def check_dox(out: Path, expected: int) -> tuple[list[tuple[str, str, float | None]], str]:
    """Per file (path, status, tool-reported ms). status: ok | incomplete | not_processed."""
    rows = []
    try:
        with out.open("rb") as f:
            for line in f:
                if not line.strip():
                    continue
                d = json.loads(line)
                if not d.get("processed"):
                    st = "not_processed"
                elif d.get("summary", {}).get("exit_code") == 2:
                    st = "incomplete"
                else:
                    st = "ok"
                rows.append((d["input"]["path"], st, d.get("timing_ms", {}).get("total")))
    except (OSError, ValueError, KeyError) as e:
        return rows, f"report unreadable: {e!r}"[:200]
    return rows, "" if len(rows) == expected else f"{len(rows)} of {expected} reports"


def check_verapdf(out: Path, expected: int) -> tuple[list[tuple[str, str, float | None]], str]:
    rows = []
    try:
        with out.open("rb") as f:
            d = json.load(f)
        reports = [r["report"] for r in d["reports"]] if "reports" in d else [d["report"]]
        for rep in reports:
            for job in rep.get("jobs", []):
                path = job["itemDetails"]["name"]
                ms = job.get("processingTime", {}).get("difference")
                vr = job.get("validationResult")
                if job.get("taskException") or not vr:
                    te = job.get("taskException") or {}
                    st = "failed:" + str(te.get("type", "no_result"))
                elif all(v.get("jobEndStatus") == "normal" for v in vr):
                    st = "ok"
                else:
                    st = "failed:" + ",".join(sorted({str(v.get("jobEndStatus")) for v in vr}))
                rows.append((path, st, ms))
    except (OSError, ValueError, KeyError, TypeError) as e:
        return rows, f"report unreadable: {e!r}"[:200]
    return rows, "" if len(rows) == expected else f"{len(rows)} of {expected} reports"


def check_cpdf(out: Path, expected: int, exit_code: int, path: str) -> tuple[list[tuple[str, str, float | None]], str]:
    """cpdf prints a JSON array of failed Matterhorn checks (exit 0) or fails to read the file (exit 2)."""
    try:
        ok = isinstance(json.loads(out.read_bytes()), list) and exit_code == 0
    except (OSError, ValueError):
        ok = False
    return [(path, "ok" if ok else f"failed:exit{exit_code}", None)], ""


def check_results_list(variant: str, out: Path, expected: int) -> tuple[list[tuple[str, str, float | None]], str]:
    """vellum-preflight (one report, or {"results": [...]}) and outloud ({"results": [...]}): one entry per file."""
    rows = []
    try:
        d = json.loads(out.read_bytes())
        for it in d["results"] if "results" in d else [d]:
            if variant == "vellum-ua1":
                st = "ok" if isinstance(it.get("conformant"), bool) else "failed:" + str(it.get("error") or "no_result")[:60]
                rows.append((it.get("file", ""), st, None))
            else:
                st = "ok" if not it.get("error") else "failed:" + str(it["error"])[:60]
                ms = it["seconds"] * 1000 if isinstance(it.get("seconds"), (int, float)) else None
                rows.append((it.get("path", ""), st, ms))
    except (OSError, ValueError, KeyError, TypeError) as e:
        return rows, f"report unreadable: {e!r}"[:200]
    return rows, "" if len(rows) == expected else f"{len(rows)} of {expected} reports"


def check(variant: str, out: Path, expected: int, exit_code: int = 0, files: list[str] | None = None):
    if variant in ("vellum-ua1", "outloud-conf"):
        return check_results_list(variant, out, expected)
    if variant.startswith("cpdf"):
        return check_cpdf(out, expected, exit_code, (files or [""])[0])
    return (check_dox if variant.startswith("dox") else check_verapdf)(out, expected)


# ---------------------------------------------------------------------------------------------------------- measurement

def parse_cpus(s: str) -> list[int]:
    cpus: list[int] = []
    for part in s.split(","):
        a, _, b = part.partition("-")
        cpus.extend(range(int(a), int(b or a) + 1))
    return cpus


def cpu_busy_ticks(cpus: list[int]) -> int:
    want = {f"cpu{c}" for c in cpus}
    total = 0
    with open("/proc/stat") as f:
        for line in f:
            p = line.split()
            if p[0] in want:
                user, nice, system, idle, iowait, irq, softirq, steal = map(int, p[1:9])
                total += user + nice + system + irq + softirq + steal
    return total


def tree_rss_bytes(root: int) -> int:
    pids, i, total = [root], 0, 0
    while i < len(pids):
        p = pids[i]
        i += 1
        try:
            for t in os.listdir(f"/proc/{p}/task"):
                with open(f"/proc/{p}/task/{t}/children") as f:
                    pids.extend(int(x) for x in f.read().split())
            with open(f"/proc/{p}/statm") as f:
                total += int(f.read().split()[1]) * PAGE
        except (OSError, ValueError):
            pass
    return total


class Sampler:
    """Samples the summed RSS of the process trees rooted at the given pids (pinned to a CPU outside the set)."""

    def __init__(self, cpu: int | None, interval: float = 0.02):
        self.roots: set[int] = set()
        self.peak = 0
        self.lock = threading.Lock()
        self.stop_ev = threading.Event()
        self.cpu = cpu
        self.interval = interval
        self.t = threading.Thread(target=self.run, daemon=True)
        self.t.start()

    def run(self):
        if self.cpu is not None:
            try:
                os.sched_setaffinity(0, {self.cpu})
            except OSError:
                pass
        while not self.stop_ev.wait(self.interval):
            with self.lock:
                roots = list(self.roots)
            if roots:
                s = sum(tree_rss_bytes(r) for r in roots)
                self.peak = max(self.peak, s)

    def stop(self) -> int:
        self.stop_ev.set()
        self.t.join()
        return self.peak


def spawn(argv: list[str], out: Path, err: Path, res: Path) -> subprocess.Popen:
    """Starts argv under rusage_exec (which writes wall/CPU/maxrss of the tool process to res)."""
    res.unlink(missing_ok=True)
    with out.open("wb") as o, err.open("wb") as e:
        return subprocess.Popen([str(RUSAGE_EXEC), str(res), *argv], stdout=o, stderr=e, stdin=subprocess.DEVNULL,
                                env=ENV, close_fds=True, start_new_session=True)


def read_res(res: Path, status: int, wall_ns: int, ru) -> dict:
    """Measurement of the tool process from rusage_exec; falls back to the harness view if it was killed."""
    try:
        w, ut, st, rss, ws = res.read_text().split()
        return {"wall_s": int(w) / 1e9, "user_s": int(ut) / 1e6, "sys_s": int(st) / 1e6, "maxrss_kb": int(rss),
                "exit": os.waitstatus_to_exitcode(int(ws))}
    except (OSError, ValueError):
        return {"wall_s": wall_ns / 1e9, "user_s": ru.ru_utime, "sys_s": ru.ru_stime, "maxrss_kb": "",
                "exit": os.waitstatus_to_exitcode(status)}


def run_one(variant: str, files: list[str], workers: int, scratch: Path, cpus: list[int], timeout: float,
            sample_cpu: int | None) -> dict:
    """One tool process over files. Returns the measurement record (+ per-file rows under '_files')."""
    out, err, res = scratch / "out.json", scratch / "err.txt", scratch / "res.txt"
    argv = argv_for(variant, files, workers)
    sampler = Sampler(sample_cpu) if sample_cpu is not None else None
    busy0 = cpu_busy_ticks(cpus)
    t0 = time.perf_counter_ns()
    p = spawn(argv, out, err, res)
    if sampler:
        with sampler.lock:
            sampler.roots.add(p.pid)
    timed_out = threading.Event()

    def kill():
        timed_out.set()
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except OSError:
            pass

    timer = threading.Timer(timeout, kill)
    timer.start()
    _, status, ru = os.wait4(p.pid, 0)
    t1 = time.perf_counter_ns()
    timer.cancel()
    p.returncode = os.waitstatus_to_exitcode(status)
    busy1 = cpu_busy_ticks(cpus)
    tree_peak = sampler.stop() if sampler else None
    child_cpu = ru.ru_utime + ru.ru_stime
    m = read_res(res, status, t1 - t0, ru)
    rows, problem = check(variant, out, len(files), m["exit"], files)
    rec = {
        "variant": variant, "workers": workers, "n_files": len(files), **m,
        "tree_rss_kb": tree_peak // 1024 if tree_peak is not None else "",
        "timeout": int(timed_out.is_set()),
        "n_ok": sum(1 for r in rows if r[1] == "ok"),
        "n_incomplete": sum(1 for r in rows if r[1] == "incomplete"),
        "n_failed": len(files) - sum(1 for r in rows if r[1] in ("ok", "incomplete")),
        "foreign_cpu_s": round(max(0.0, (busy1 - busy0) / CLK_TCK - child_cpu), 3),
        "load1": os.getloadavg()[0], "problem": problem,
        "report_bytes": out.stat().st_size if out.exists() else 0,
    }
    if p.returncode < 0 or problem:
        rec["stderr_tail"] = err.read_text(errors="replace")[-300:].replace("\n", " | ") if err.exists() else ""
    rec["_files"] = rows
    return rec


def run_pool(variant: str, files: list[str], nworkers: int, scratch: Path, cpus: list[int], timeout: float,
             sample_cpu: int | None) -> dict:
    """nworkers concurrent one-file processes until all files are done (xargs -P style); timeout per process."""
    pending = list(files)
    running: dict[int, tuple] = {}
    sampler = Sampler(sample_cpu) if sample_cpu is not None else None
    busy0 = cpu_busy_ticks(cpus)
    utime = stime = 0.0
    maxrss = 0
    rows_all: list = []
    fails = 0
    slot_free = list(range(nworkers))
    t0 = time.perf_counter_ns()
    timers: dict[int, threading.Timer] = {}
    killed: set[int] = set()

    def kill(pid: int):
        killed.add(pid)
        try:
            os.killpg(pid, signal.SIGKILL)
        except OSError:
            pass

    while pending or running:
        while pending and slot_free:
            f = pending.pop(0)
            slot = slot_free.pop()
            out, res = scratch / f"out{slot}.json", scratch / f"res{slot}.txt"
            p = spawn(argv_for(variant, [f], 1), out, scratch / f"err{slot}.txt", res)
            running[p.pid] = (p, f, out, slot, res, time.perf_counter_ns())
            timers[p.pid] = threading.Timer(timeout, kill, (p.pid,))
            timers[p.pid].start()
            if sampler:
                with sampler.lock:
                    sampler.roots.add(p.pid)
        pid, status, ru = os.wait4(-1, 0)
        if pid not in running:
            continue
        timers.pop(pid).cancel()
        p, f, out, slot, res, started = running.pop(pid)
        if sampler:
            with sampler.lock:
                sampler.roots.discard(pid)
        p.returncode = os.waitstatus_to_exitcode(status)
        m = read_res(res, status, time.perf_counter_ns() - started, ru)
        utime += m["user_s"]
        stime += m["sys_s"]
        maxrss = max(maxrss, m["maxrss_kb"] or 0)
        rows, problem = check(variant, out, 1, m["exit"], [f])
        if pid in killed:
            rows = [(f, "timeout", None)]
        elif problem or not rows:
            fails += 1
        rows_all.extend(rows)
        slot_free.append(slot)
    t1 = time.perf_counter_ns()
    busy1 = cpu_busy_ticks(cpus)
    tree_peak = sampler.stop() if sampler else None
    n_ok = sum(1 for r in rows_all if r[1] == "ok")
    n_inc = sum(1 for r in rows_all if r[1] == "incomplete")
    return {
        "variant": variant, "workers": nworkers, "n_files": len(files),
        "wall_s": (t1 - t0) / 1e9, "user_s": utime, "sys_s": stime, "maxrss_kb": maxrss,
        "tree_rss_kb": tree_peak // 1024 if tree_peak is not None else "",
        "exit": "", "timeout": 0, "n_file_timeouts": len(killed),
        "n_ok": n_ok, "n_incomplete": n_inc, "n_failed": len(files) - n_ok - n_inc,
        "foreign_cpu_s": round(max(0.0, (busy1 - busy0) / CLK_TCK - utime - stime), 3),
        "load1": os.getloadavg()[0], "problem": f"{fails} unreadable reports" if fails else "",
        "report_bytes": "", "_files": rows_all,
    }


# -------------------------------------------------------------------------------------------------------------- helpers

FIELDS = ["block", "rep", "seq", "variant", "item", "workers", "n_files", "pages", "wall_s", "user_s", "sys_s",
          "maxrss_kb", "tree_rss_kb", "exit", "timeout", "n_file_timeouts", "n_ok", "n_incomplete", "n_failed", "foreign_cpu_s", "load1",
          "report_bytes", "problem", "stderr_tail"]


class Sink:
    def __init__(self, name: str, fields=FIELDS):
        RESULTS.mkdir(parents=True, exist_ok=True)
        self.path = RESULTS / f"{name}.csv"
        self.f = self.path.open("w", newline="")
        self.w = csv.DictWriter(self.f, fieldnames=fields, extrasaction="ignore")
        self.w.writeheader()

    def put(self, row: dict):
        r = {k: (f"{v:.6f}" if isinstance(v, float) and k.endswith("_s") else v) for k, v in row.items()}
        self.w.writerow(r)
        self.f.flush()


def load_snapshot(label: str) -> dict:
    top = subprocess.run(["ps", "-eo", "pcpu,rss,psr,comm", "--sort=-pcpu", "--no-headers"], capture_output=True,
                         text=True).stdout.splitlines()[:6]
    return {"label": label, "time": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "loadavg": open("/proc/loadavg").read().strip(),
            "top": [" ".join(l.split()) for l in top]}


def log_load(block: str, label: str):
    with (RESULTS / "load.jsonl").open("a") as f:
        f.write(json.dumps({"block": block, **load_snapshot(label)}) + "\n")


def warm_page_cache(files: list[str]):
    for f in files:
        with open(f, "rb") as fh:
            while fh.read(1 << 20):
                pass


def load_corpus(corpus_dir: Path, require_ready: bool) -> tuple[list[dict], str]:
    """Reads corpus_manifest.json (paths relative to its directory) and checks it against CORPUS_READY (searched in the
    corpus directory and its parents) and every file against its sha256 in the manifest."""
    manifest = corpus_dir / "corpus_manifest.json"
    raw = manifest.read_bytes()
    sha = hashlib.sha256(raw).hexdigest()
    ready = next((d / "CORPUS_READY" for d in [corpus_dir, *corpus_dir.parents] if (d / "CORPUS_READY").exists()), None)
    if require_ready:
        if not ready:
            sys.exit(f"no CORPUS_READY above {corpus_dir}: the corpus is not frozen (--allow-unfrozen only for script tests)")
        if sha not in ready.read_text():
            sys.exit(f"corpus_manifest.json sha256 {sha} does not match {ready}")
    d = json.loads(raw)
    entries = d["files"] if isinstance(d, dict) and "files" in d else d
    docs = []
    for e in entries:
        p = Path(e["path"])
        if not p.is_absolute():
            p = corpus_dir / p
        docs.append({"id": str(e.get("id") or e["path"]), "path": str(p), "pages": int(e.get("pages") or 0),
                     "bytes": int(e.get("bytes") or 0), "tier": e.get("size_tier") or e.get("tier") or "",
                     "complexity": e.get("complexity") or "", "origin": e.get("origin") or "",
                     "group": e.get("group") or "", "sha256": e.get("sha256") or ""})
    for doc in docs:
        if not os.path.isfile(doc["path"]):
            sys.exit(f"corpus file missing: {doc['path']}")
        if doc["sha256"] and hashlib.sha256(Path(doc["path"]).read_bytes()).hexdigest() != doc["sha256"]:
            sys.exit(f"corpus file differs from the manifest: {doc['path']}")
    return docs, sha


def rotated(seq: list, k: int) -> list:
    k %= len(seq)
    return seq[k:] + seq[:k]


# ---------------------------------------------------------------------------------------------------------------- blocks

def block_startup(a, docs):
    sink = Sink("startup")
    smallest = min((d for d in docs if d["pages"] > 0), key=lambda d: (d["bytes"], d["id"]))
    variants = ["dox", "vp-ua1", "vp1.30.2-ua1", "cpdf-ua1"]
    log_load("startup", "before")
    seq = 0
    for rep in range(a.reps_startup):
        for v in rotated(variants, rep):
            for item in ("--version", "smallest"):
                if item == "--version":
                    exe = argv_for(v, [])[0]
                    res = a.scratch / "res.txt"
                    t0 = time.perf_counter_ns()
                    p = spawn([exe, "--version"], a.scratch / "out.txt", a.scratch / "err.txt", res)
                    _, st, ru = os.wait4(p.pid, 0)
                    t1 = time.perf_counter_ns()
                    rec = {"variant": v, "workers": 1, "n_files": 0, **read_res(res, st, t1 - t0, ru),
                           "load1": os.getloadavg()[0]}
                else:
                    rec = run_one(v, [smallest["path"]], 1, a.scratch, a.cpus, a.timeout, None)
                    rec.pop("_files")
                seq += 1
                sink.put({"block": "startup", "rep": rep, "seq": seq, "item": item if item == "--version" else smallest["id"],
                          "pages": smallest["pages"] if item != "--version" else 0, **rec})
    log_load("startup", "after")


def block_cold(a, docs):
    sink = Sink("cold")
    variants = a.cold_variants
    rng = random.Random(1)
    log_load("cold", "before")
    seq = 0
    for rep in range(a.reps):
        order = docs[:]
        rng.shuffle(order)
        for i, doc in enumerate(order):
            for v in rotated(variants, rep + i):
                rec = run_one(v, [doc["path"]], 1, a.scratch, a.cpus, a.timeout, None)
                if rec["n_failed"] and rec["exit"] is not None and isinstance(rec["exit"], int) and rec["exit"] < 0:
                    rec["problem"] = (rec["problem"] + f" signal {-rec['exit']}").strip()
                rec.pop("_files")
                seq += 1
                sink.put({"block": "cold", "rep": rep, "seq": seq, "item": doc["id"], "pages": doc["pages"], **rec})
        log_load("cold", f"after rep {rep}")


def block_batch(a, docs):
    sink = Sink("batch")
    fsink = Sink("batch_files", ["rep", "variant", "pass", "item", "status", "tool_ms"])
    by_path = {d["path"]: d for d in docs}
    paths = [d["path"] for d in docs]
    pages = sum(d["pages"] for d in docs)
    log_load("batch", "before")
    seq = 0
    for rep in range(a.reps):
        for v in rotated(a.batch_variants, rep):
            files = paths * a.passes
            rec = run_one(v, files, 1, a.scratch, a.cpus, a.corpus_timeout * a.passes, None)
            rows = rec.pop("_files")
            seq += 1
            sink.put({"block": "batch", "rep": rep, "seq": seq, "item": f"corpus x{a.passes}", "pages": pages * a.passes, **rec})
            n = len(paths)
            for k, (path, st, ms) in enumerate(rows):
                d = by_path.get(path) or by_path.get(os.path.abspath(path))
                fsink.put({"rep": rep, "variant": v, "pass": k // n if len(rows) == len(files) else "",
                           "item": d["id"] if d else path, "status": st, "tool_ms": ms})
        log_load("batch", f"after rep {rep}")


def block_parallel(a, docs):
    sink = Sink("parallel")
    paths = [d["path"] for d in docs]
    pages = sum(d["pages"] for d in docs)
    log_load("parallel", "before")
    seq = 0
    configs = [("native", v, w) for w in a.workers for v in a.parallel_variants if not v.startswith("cpdf")]
    configs += [("pool", v, w) for w in a.workers for v in a.pool_variants]
    configs.sort(key=lambda c: c[2])
    for rep in range(a.reps):
        for mode, v, w in rotated(configs, rep):
            if w > len(a.cpus):
                continue
            if mode == "native":
                rec = run_one(v, paths, w, a.scratch, a.cpus, a.corpus_timeout, a.sample_cpu)
            else:
                rec = run_pool(v, paths, w, a.scratch, a.cpus, a.timeout, a.sample_cpu)
            rec.pop("_files")
            seq += 1
            sink.put({"block": f"parallel-{mode}", "rep": rep, "seq": seq, "item": "corpus", "pages": pages, **rec})
        log_load("parallel", f"after rep {rep}")


# ------------------------------------------------------------------------------------------------------------------- env

def sh(*argv, **kw) -> str:
    try:
        r = subprocess.run(list(argv), capture_output=True, text=True, timeout=120, **kw)
        return (r.stdout + r.stderr).strip()
    except (OSError, subprocess.TimeoutExpired) as e:
        return f"unavailable: {e}"


def read(path: str) -> str:
    try:
        return Path(path).read_text().strip()
    except OSError:
        return ""


def block_env(a, docs, corpus_sha):
    cpuinfo = read("/proc/cpuinfo")
    model = re.search(r"model name\s*:\s*(.*)", cpuinfo)
    mem = re.search(r"MemTotal:\s*(\d+)", read("/proc/meminfo"))
    os_rel = re.search(r'PRETTY_NAME="(.*)"', read("/etc/os-release"))
    flags_line = ""
    ninja = ROOT / "build-bench" / "build.ninja"
    if ninja.exists():
        m = re.search(r"^\s*FLAGS = (.*)$", ninja.read_text(), re.M)
        flags_line = m.group(1) if m else ""
    jvm_flags = sh(str(JRE / "bin" / "java"), "-XX:+PrintFlagsFinal", "-version")
    wanted = ("UseG1GC", "UseSerialGC", "UseParallelGC", "MaxHeapSize", "InitialHeapSize", "ParallelGCThreads",
              "ConcGCThreads", "CICompilerCount", "TieredCompilation", "UseCompressedOops")
    jflags = {}
    for line in jvm_flags.splitlines():
        p = line.split()
        if len(p) >= 4 and p[1] in wanted:
            jflags[p[1]] = p[3]
    vp_launcher = VERAPDF["1.30.4"].read_text() if VERAPDF["1.30.4"].exists() else ""
    m = re.search(r'exec "\$JAVACMD" (.*?)org\.verapdf', vp_launcher, re.S)
    env = {
        "date": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "cpu_model": model.group(1) if model else "",
        "cpu_set": a.cpus_str, "sampler_cpu": a.sample_cpu,
        "cpu_set_max_freq_khz": {c: read(f"/sys/devices/system/cpu/cpu{c}/cpufreq/cpuinfo_max_freq") for c in a.cpus},
        "cpu_set_siblings": sorted({read(f"/sys/devices/system/cpu/cpu{c}/topology/thread_siblings_list") for c in a.cpus}),
        "smt_active": read("/sys/devices/system/cpu/smt/active"),
        "governor": read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"),
        "cpufreq_driver": read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_driver"),
        "energy_performance_preference": read("/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference"),
        "intel_pstate_no_turbo": read("/sys/devices/system/cpu/intel_pstate/no_turbo"),
        "logical_cpus": os.cpu_count(),
        "niceness": os.nice(0),
        "harness_affinity": sorted(os.sched_getaffinity(0)),
        "ram_kb": int(mem.group(1)) if mem else None,
        "os": os_rel.group(1) if os_rel else "", "kernel": os.uname().release,
        "python": sys.version.split()[0],
        "compiler": sh("c++", "--version").splitlines()[0],
        "qpdf": sh("pkg-config", "--modversion", "libqpdf"),
        "pugixml": sh("pkg-config", "--modversion", "pugixml"),
        "zlib": sh("pkg-config", "--modversion", "zlib"),
        "dox_cxx_flags": flags_line,
        "dox_version": sh(str(DOX), "--version"),
        "dox_commit": sh("git", "-C", str(ROOT), "rev-parse", "HEAD"),
        "dox_tree_dirty_src": bool(sh("git", "-C", str(ROOT), "status", "--porcelain", "--", "src", "catalog", "CMakeLists.txt")),
        "dox_binary_sha256": hashlib.sha256(DOX.read_bytes()).hexdigest() if DOX.exists() else "",
        "verapdf": {v: sh(str(p), "--version", env=ENV).splitlines()[0] for v, p in VERAPDF.items() if p.exists()},
        "verapdf_parser": "greenfield (veraPDF's own parser; org.verapdf.apps.GreenfieldCliWrapper)",
        "verapdf_launcher_jvm_args": " ".join(m.group(1).split()) if m else "",
        "verapdf_user_config": {f.name: f.read_text().strip() for f in sorted((Path.home() / ".verapdf" / "config").glob("*.xml"))},
        "java": sh(str(JRE / "bin" / "java"), "-version"),
        "cpdf": sh(str(CPDF), "-version").splitlines()[0],
        "java_opts": "none (JAVA_OPTS/JAVA_TOOL_OPTIONS unset; JVM defaults chosen by HotSpot ergonomics)",
        "jvm_ergonomics_on_cpu_set": jflags,
        "corpus_manifest_sha256": corpus_sha,
        "corpus_ready": next((read(str(d / "CORPUS_READY")) for d in [a.corpus, *a.corpus.parents]
                              if (d / "CORPUS_READY").exists()), ""),
        "corpus_files": len(docs), "corpus_pages": sum(d["pages"] for d in docs), "corpus_bytes": sum(d["bytes"] for d in docs),
        "settings": {"reps": a.reps, "passes": a.passes, "workers": a.workers, "timeout_s": a.timeout, "corpus_timeout_s": a.corpus_timeout,
                     "cold_variants": a.cold_variants, "batch_variants": a.batch_variants,
                     "parallel_variants": a.parallel_variants, "pool_variants": a.pool_variants,
                     "report_dir": "tmpfs (/dev/shm)"},
        "load_now": load_snapshot("env"),
    }
    RESULTS.mkdir(parents=True, exist_ok=True)
    (RESULTS / "env.json").write_text(json.dumps(env, indent=1) + "\n")
    # The corpus description used by the analysis (ids, pages, tiers; no paths outside the repository).
    with (RESULTS / "corpus.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["id", "pages", "bytes", "tier", "complexity", "origin", "group"],
                           extrasaction="ignore")
        w.writeheader()
        for d in docs:
            w.writerow(d)


# ------------------------------------------------------------------------------------------------------------------ main

ENV = tool_env()


def main():
    global RESULTS
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("blocks", nargs="+", choices=["env", "startup", "cold", "batch", "parallel"])
    ap.add_argument("--corpus", type=Path, default=ROOT / "tests" / "corpus" / ".cache" / "full",
                    help="directory with corpus_manifest.json (default: the frozen corpus built by tests/corpus/build_corpus.py)")
    ap.add_argument("--allow-unfrozen", action="store_true", help="script test without CORPUS_READY (results are not valid)")
    ap.add_argument("--cpus", default="0-7", help="CPU set for the tool processes (default 0-7)")
    ap.add_argument("--sample-cpu", type=int, default=16, help="CPU for the RSS sampler thread (outside the set)")
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--reps-startup", type=int, default=10)
    ap.add_argument("--passes", type=int, default=3, help="passes over the corpus in one process (batch block)")
    ap.add_argument("--workers", default="1,2,4,8")
    ap.add_argument("--timeout", type=float, default=120.0,
                    help="seconds per one-document process (cold, startup, pool); about 10x the slowest veraPDF document")
    ap.add_argument("--corpus-timeout", type=float, default=1800.0,
                    help="seconds per pass for whole-corpus processes (batch, native parallel)")
    ap.add_argument("--cold-variants", default="dox,dox-ua1rules,vp-ua1,vp-ua2,vp-wt1a,vp-wt1r,cpdf-ua1")
    ap.add_argument("--batch-variants", default="dox,dox-ua1rules,vp-ua1,vp-ua2,vp-wt1a,vp-wt1r,vp1.30.2-ua1")
    ap.add_argument("--parallel-variants", default="dox,vp-ua1", help="tool-internal parallelism (--jobs / --processes)")
    ap.add_argument("--pool-variants", default="dox,vp-ua1", help="N concurrent one-file processes")
    ap.add_argument("--limit", type=int, default=0, help="use only the first N corpus files (script tests)")
    ap.add_argument("--results", type=Path, default=None, help="output directory (default bench/results)")
    a = ap.parse_args()

    if a.results:
        RESULTS = a.results
    a.cpus_str = a.cpus
    a.cpus = parse_cpus(a.cpus)
    a.workers = [int(x) for x in a.workers.split(",")]
    a.cold_variants = a.cold_variants.split(",")
    a.batch_variants = a.batch_variants.split(",")
    a.parallel_variants = a.parallel_variants.split(",")
    a.pool_variants = a.pool_variants.split(",")
    if a.sample_cpu in a.cpus:
        sys.exit("--sample-cpu must be outside --cpus")
    for p in [DOX, RUSAGE_EXEC, CPDF, JRE / "bin" / "java", *VERAPDF.values()]:
        if not p.exists():
            sys.exit(f"missing {p}: run bench/setup_tools.sh")
    docs, sha = load_corpus(a.corpus, not a.allow_unfrozen)
    docs.sort(key=lambda d: d["id"])
    if a.limit:
        docs = docs[: a.limit]
    a.scratch = Path("/dev/shm") / f"dox-bench-{os.getpid()}"
    a.scratch.mkdir(parents=True, exist_ok=True)
    os.sched_setaffinity(0, set(a.cpus))   # the spawning thread; tool processes inherit this CPU set
    try:
        warm_page_cache([d["path"] for d in docs])
        for b in a.blocks:
            t = time.time()
            print(f"[{time.strftime('%H:%M:%S')}] block {b} ({len(docs)} files)", flush=True)
            if b == "env":
                block_env(a, docs, sha)
            else:
                warm_page_cache([d["path"] for d in docs])
                globals()[f"block_{b}"](a, docs)
            print(f"[{time.strftime('%H:%M:%S')}] block {b} done in {time.time() - t:.0f} s", flush=True)
    finally:
        shutil.rmtree(a.scratch, ignore_errors=True)


if __name__ == "__main__":
    main()
