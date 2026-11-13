#!/usr/bin/env python3
"""Materialises the full test corpus in one directory, with corpus_manifest.json.

  build_corpus.py [--out DIR] [--suites] [--no-fetch]

Contents (paths relative to DIR):
  fixtures/   the committed synthetic fixtures (tests/corpus/fixtures, expectations in expected.json)
  generated/  the on-demand size tiers (generate.py --large; deterministic, verified against generated.json)
  public/     the public documents of tests/corpus/public/manifest.json marked "core" (downloaded and sha256-verified
              by fetch_public.py); with --suites also every veraPDF PDF/UA-1 and PDF/UA-2 corpus file

corpus_manifest.json lists every file: path, sha256, bytes, pages, size tier (small <= 10 pages, medium 11-299,
large >= 300; unknown for files that are not readable PDFs), complexity (complex = tagged with a structure tree,
simple = untagged), origin (generated / public), tagged (yes / no / unknown) and the source of its expectation. The
manifest has no timestamps or absolute paths: the same inputs give the same bytes, so its sha256 identifies the corpus.
Default DIR: tests/corpus/.cache/full. Standard library only.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIXTURES = HERE / "fixtures"
GENERATED = HERE / ".cache" / "generated"
PUBLIC = HERE / ".cache" / "public"
MANIFEST = HERE / "public" / "manifest.json"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def tier(pages: int | None) -> str:
    if not pages:
        return "unknown"
    return "small" if pages <= 10 else "medium" if pages < 300 else "large"


def own_page_count(data: bytes) -> int | None:
    """Page count of a file written by pdfgen (uncompressed, one /Type /Pages root with /Count)."""
    m = re.search(rb"/Type /Pages /Kids \[[^\]]*\] /Count (\d+)", data)
    return int(m.group(1)) if m else None


def copy(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    if not dst.exists() or dst.stat().st_size != src.stat().st_size or sha256(dst) != sha256(src):
        shutil.copyfile(src, dst)


def yesno(v) -> str:
    return "unknown" if v is None else "yes" if v else "no"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=HERE / ".cache" / "full")
    ap.add_argument("--suites", action="store_true", help="include the complete veraPDF PDF/UA-1 and PDF/UA-2 suites")
    ap.add_argument("--no-fetch", action="store_true", help="fail instead of downloading missing public documents")
    a = ap.parse_args()
    out: Path = a.out
    entries = []

    # 1. committed fixtures (verify that they are what the generator produces)
    check = subprocess.run([sys.executable, str(HERE / "generate.py"), "--check"], capture_output=True, text=True)
    if check.returncode != 0:
        sys.exit("committed fixtures do not reproduce:\n" + check.stdout)
    for spec in json.loads((FIXTURES / "expected.json").read_text())["fixtures"]:
        src = FIXTURES / spec["file"]
        data = src.read_bytes()
        assert hashlib.sha256(data).hexdigest() == spec["sha256"], src
        pages = own_page_count(data) if spec["processed"] else None
        copy(src, out / "fixtures" / spec["file"])
        entries.append({"path": f"fixtures/{spec['file']}", "id": spec["id"], "sha256": spec["sha256"], "bytes": len(data),
                        "pages": pages, "size_tier": tier(pages) if pages else "small" if data else "unknown",
                        "complexity": "complex" if spec["tagged"] else "simple", "origin": "generated",
                        "tagged": yesno(spec["tagged"]), "group": "fixture",
                        "expectation": "tests/corpus/fixtures/expected.json"})

    # 2. generated size tiers
    gen_json = GENERATED / "generated.json"
    if not gen_json.exists():
        subprocess.run([sys.executable, str(HERE / "generate.py"), "--large"], check=True)
    for spec in json.loads(gen_json.read_text())["fixtures"]:
        src = GENERATED / spec["file"]
        if not src.exists() or sha256(src) != spec["sha256"]:
            subprocess.run([sys.executable, str(HERE / "generate.py"), "--large", "--only", spec["id"]], check=True)
            if sha256(src) != spec["sha256"]:
                sys.exit(f"{src}: the generator does not reproduce the recorded sha256")
        copy(src, out / "generated" / spec["file"])
        entries.append({"path": f"generated/{spec['file']}", "id": spec["id"], "sha256": spec["sha256"], "bytes": spec["bytes"],
                        "pages": spec["pages"], "size_tier": tier(spec["pages"]), "complexity": spec["complexity"],
                        "origin": "generated", "tagged": yesno(spec["tagged"]), "group": "generated",
                        "expectation": "tests/corpus/.cache/generated/generated.json (generate.py)"})

    # 3. public documents
    docs = json.loads(MANIFEST.read_text())["documents"]
    wanted = [d for d in docs if d.get("core") or (a.suites and d["group"].startswith("verapdf-"))]
    missing = [d for d in wanted if not (PUBLIC / d["local"]).exists() or sha256(PUBLIC / d["local"]) != d["sha256"]]
    if missing:
        if a.no_fetch:
            sys.exit(f"{len(missing)} public document(s) missing; run tests/corpus/fetch_public.py")
        cmd = [sys.executable, str(HERE / "fetch_public.py")] + [x for d in missing for x in ("--id", d["id"])]
        subprocess.run(cmd, check=True)
    for d in wanted:
        src = PUBLIC / d["local"]
        if sha256(src) != d["sha256"]:
            sys.exit(f"{src}: sha256 mismatch")
        copy(src, out / "public" / d["local"])
        entries.append({"path": f"public/{d['local']}", "id": d["id"], "sha256": d["sha256"], "bytes": d["bytes"],
                        "pages": d["pages"], "size_tier": tier(d["pages"]), "complexity": "complex" if d["tagged"] else "simple",
                        "origin": "public", "tagged": yesno(d["tagged"]), "group": d["group"],
                        "expectation": "tests/corpus/public/manifest.json (independent_expectation)"})

    entries.sort(key=lambda e: e["path"])
    manifest = {"schema": "dox-validate-corpus/1",
                "build_command": "python3 tests/corpus/build_corpus.py" + (" --suites" if a.suites else ""),
                "counts": {k: sum(1 for e in entries if e[f] == k) for f in ("size_tier", "origin")
                           for k in sorted({e[f] for e in entries})},
                "files": entries}
    data = (json.dumps(manifest, indent=1) + "\n").encode()
    (out / "corpus_manifest.json").write_bytes(data)
    print(f"{len(entries)} files in {out}; corpus_manifest.json sha256 {hashlib.sha256(data).hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
