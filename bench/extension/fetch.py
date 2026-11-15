#!/usr/bin/env python3
"""Materialises the benchmark extension: the public documents of bench/extension/manifest.json plus the anchor
documents of the frozen test corpus, with a corpus_manifest.json that bench/bench.py reads.

  python3 -I bench/extension/fetch.py [--from DIR]

Output: bench/extension/.cache/ (gitignored)
  extension/<id>.pdf      downloaded from the manifest URL (or taken from DIR when a file there has the right sha256)
  anchor/<id>.pdf         copied from tests/corpus/.cache/full (build that first: python3 tests/corpus/build_corpus.py)
  corpus_manifest.json    path, id, sha256, bytes, pages, size tier, complexity, origin, group of every file

Every file is checked against the sha256 in the manifest; a mismatch stops the script. corpus_manifest.json has no
timestamps or absolute paths, so its sha256 identifies the set; bench.py refuses to run unless that sha256 is in
bench/extension/CORPUS_READY. Standard library only.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import ssl
import sys
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
OUT = HERE / ".cache"
FULL = ROOT / "tests" / "corpus" / ".cache" / "full"
UA = "Mozilla/5.0 (X11; Linux x86_64) corpus-fetch/1.0"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def ssl_context() -> ssl.SSLContext:
    """The default context; when Python's own CA paths are empty (some relocated interpreters), the system bundle."""
    ctx = ssl.create_default_context(cafile=os.environ.get("SSL_CERT_FILE"))
    if not ctx.get_ca_certs():
        for bundle in ("/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/cert.pem"):
            if Path(bundle).exists():
                ctx.load_verify_locations(bundle)
                break
    return ctx


def download(url: str, target: Path, tries: int = 3) -> None:
    part = target.with_suffix(".part")
    for attempt in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=180, context=ssl_context()) as r, part.open("wb") as f:
                shutil.copyfileobj(r, f)
            part.rename(target)
            return
        except Exception as e:  # noqa: BLE001 - retried, then reported
            if attempt == tries - 1:
                raise RuntimeError(f"{url}: {e}") from e
            time.sleep(2 * (attempt + 1))


def tier(pages: int) -> str:   # as tests/corpus/build_corpus.py
    return "small" if pages <= 10 else "medium" if pages < 300 else "large"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--from", dest="src", type=Path, help="directory with already downloaded files (<id>.pdf)")
    a = ap.parse_args()
    m = json.loads((HERE / "manifest.json").read_text())
    entries = []
    for d in m["documents"]:
        target = OUT / "extension" / f"{d['id']}.pdf"
        target.parent.mkdir(parents=True, exist_ok=True)
        if not (target.exists() and sha256(target) == d["sha256"]):
            local = a.src / f"{d['id']}.pdf" if a.src else None
            if local and local.exists() and sha256(local) == d["sha256"]:
                shutil.copyfile(local, target)
            else:
                print(f"downloading {d['url']}", flush=True)
                download(d["url"], target)
                time.sleep(1)
        if sha256(target) != d["sha256"]:
            sys.exit(f"{d['id']}: sha256 differs from the manifest (the publisher may have replaced the file)")
        entries.append({"path": f"extension/{d['id']}.pdf", "id": d["id"], "sha256": d["sha256"], "bytes": d["bytes"],
                        "pages": d["pages"], "size_tier": tier(d["pages"]),
                        "complexity": "complex" if d["tagged"] else "simple", "origin": "public", "group": "extension",
                        "tagged": "yes" if d["tagged"] else "no"})
    for d in m["anchors"]:
        src = FULL / d["corpus_path"]
        if not src.exists():
            sys.exit(f"anchor {d['id']}: {src} is missing (run python3 tests/corpus/build_corpus.py)")
        target = OUT / "anchor" / f"{d['id']}.pdf"
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, target)
        if sha256(target) != d["sha256"]:
            sys.exit(f"anchor {d['id']}: sha256 differs from the manifest")
        entries.append({"path": f"anchor/{d['id']}.pdf", "id": d["id"], "sha256": d["sha256"], "bytes": d["bytes"],
                        "pages": d["pages"], "size_tier": tier(d["pages"]), "complexity": "", "origin": "test corpus",
                        "group": "anchor", "tagged": ""})
    entries.sort(key=lambda e: e["path"])
    out = {"schema": "dox-validate-benchmark-extension-files/1",
           "build_command": "python3 -I bench/extension/fetch.py",
           "counts": {"extension": len(m["documents"]), "anchor": len(m["anchors"])},
           "files": entries}
    raw = (json.dumps(out, indent=1) + "\n").encode()
    (OUT / "corpus_manifest.json").write_bytes(raw)
    digest = hashlib.sha256(raw).hexdigest()
    ready = HERE / "CORPUS_READY"
    frozen = ready.exists() and digest in ready.read_text()
    print(f"{len(entries)} files in {OUT}; corpus_manifest.json sha256 {digest}"
          f" ({'matches CORPUS_READY' if frozen else 'NOT in CORPUS_READY'})")


if __name__ == "__main__":
    main()
