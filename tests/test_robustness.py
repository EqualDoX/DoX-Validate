"""Damaged, hostile and unusual inputs never crash the validator: exit 0/1/2 only, a valid report, bounded time and memory.

Besides the malformed fixtures, deterministic mutations of a good fixture (truncations, byte flips, inserted garbage,
dropped keywords) are generated into a temporary directory.
"""
import json
import os
import random
import subprocess
import threading
import time

import pytest

from conftest import BINARY, FIXTURES, catalogue, load_expected
from report_invariants import check_report

MAX_SECONDS = 30
MAX_RSS_MB = 1024


def run_measured(pdf):
    """Exit status, stdout, stderr, wall time and peak RSS (MiB) of one validation, killed after MAX_SECONDS."""
    t0 = time.monotonic()
    p = subprocess.Popen([str(BINARY), "--format", "json", str(pdf)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    timer = threading.Timer(MAX_SECONDS, p.kill)
    timer.start()
    chunks = {}
    readers = [threading.Thread(target=lambda k=k, s=s: chunks.__setitem__(k, s.read())) for k, s in (("out", p.stdout), ("err", p.stderr))]
    for t in readers:
        t.start()
    _, status, ru = os.wait4(p.pid, 0)
    timer.cancel()
    for t in readers:
        t.join()
    p.returncode = os.waitstatus_to_exitcode(status)
    secs = time.monotonic() - t0
    assert secs < MAX_SECONDS, f"{pdf.name}: no result within {MAX_SECONDS} s"
    return p.returncode, chunks["out"], chunks["err"].decode("utf-8", "replace"), secs, ru.ru_maxrss / 1024


def assert_robust(pdf):
    code, out, err, secs, rss = run_measured(pdf)
    assert code in (0, 1, 2), f"{pdf.name}: exit status {code} (negative = killed by a signal) stderr={err[-300:]!r}"
    report = json.loads(out)
    problems = check_report(report, catalogue(), code)
    assert not problems, f"{pdf.name}: " + "; ".join(problems[:5])
    assert rss < MAX_RSS_MB, f"{pdf.name}: peak RSS {rss:.0f} MiB"


MALFORMED = [s["file"] for s in load_expected() if s["id"].startswith(("malformed-", "edge-", "encrypted-"))]


@pytest.mark.parametrize("name", MALFORMED)
def test_malformed_fixture(name):
    assert_robust(FIXTURES / name)


def mutations():
    base = (FIXTURES / "ua1-good.pdf").read_bytes()
    rnd = random.Random(20261113)
    out = []
    for k in range(8):                                         # truncations
        out.append((f"trunc-{k}", base[: rnd.randrange(1, len(base))]))
    for k in range(12):                                        # random byte flips
        b = bytearray(base)
        for _ in range(rnd.choice((1, 8, 64))):
            b[rnd.randrange(len(b))] = rnd.randrange(256)
        out.append((f"flip-{k}", bytes(b)))
    for k in range(4):                                         # garbage inserted
        i = rnd.randrange(len(base))
        out.append((f"insert-{k}", base[:i] + bytes(rnd.randrange(256) for _ in range(512)) + base[i:]))
    for kw in (b"endobj", b"endstream", b"stream", b"xref", b"trailer", b"obj"):   # every occurrence of a keyword dropped
        out.append((f"drop-{kw.decode()}", base.replace(kw, b"")))
    out.append(("deep-array", b"%PDF-1.7\n1 0 obj\n" + b"[" * 100000 + b"\nendobj\ntrailer\n<</Root 1 0 R>>\n%%EOF\n"))
    out.append(("deep-dict", b"%PDF-1.7\n1 0 obj\n" + b"<</A " * 50000 + b"\nendobj\ntrailer\n<</Root 1 0 R>>\n%%EOF\n"))
    out.append(("self-ref", b"%PDF-1.7\n1 0 obj\n<</Type /Catalog /Pages 1 0 R>>\nendobj\ntrailer\n<</Root 1 0 R>>\n%%EOF\n"))
    out.append(("pages-loop", b"%PDF-1.7\n1 0 obj\n<</Type /Catalog /Pages 2 0 R>>\nendobj\n2 0 obj\n<</Type /Pages /Kids [2 0 R] "
                b"/Count 1>>\nendobj\ntrailer\n<</Root 1 0 R>>\n%%EOF\n"))
    return out


MUT = mutations()


@pytest.mark.parametrize("name,data", MUT, ids=[m[0] for m in MUT])
def test_mutated_input(name, data, tmp_path):
    pdf = tmp_path / f"{name}.pdf"
    pdf.write_bytes(data)
    assert_robust(pdf)


def test_directory_and_unreadable_inputs(tmp_path):
    for target in (tmp_path, tmp_path / "missing.pdf"):
        p = subprocess.run([str(BINARY), "--format", "json", str(target)], capture_output=True, timeout=MAX_SECONDS)
        assert p.returncode == 2
        assert json.loads(p.stdout)["processed"] is False
