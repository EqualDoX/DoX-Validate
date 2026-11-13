#!/usr/bin/env python3
"""Downloads the public documents of the corpus (tests/corpus/public/manifest.json) and verifies their sha256.

  fetch_public.py                 the core set (documents with "core": true)
  fetch_public.py --all           every document, including the full veraPDF PDF/UA-1 and PDF/UA-2 suites
  fetch_public.py --group NAME    one group (see "groups" in the manifest); may be repeated
  fetch_public.py --id ID         one document; may be repeated
  fetch_public.py --verify        only verify what is already downloaded (no network)

Files go to tests/corpus/.cache/public/<local> (gitignored). A file whose sha256 differs from the manifest is deleted and
reported; nothing unverified is kept. Documents extracted from an archive are verified after extraction as well.
Standard library only.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import ssl
import sys
import time
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
MANIFEST = HERE / "public" / "manifest.json"
DEST = HERE / ".cache" / "public"
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
    target.parent.mkdir(parents=True, exist_ok=True)
    part = target.with_name(target.name + ".part")
    for attempt in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=120, context=ssl_context()) as r, part.open("wb") as f:
                while chunk := r.read(1 << 20):
                    f.write(chunk)
            part.replace(target)
            return
        except Exception as e:  # noqa: BLE001 - report and retry any network error
            if attempt + 1 == tries:
                part.unlink(missing_ok=True)
                raise RuntimeError(f"{url}: {e}") from e
            time.sleep(2 * (attempt + 1))


def select(docs: list[dict], groups: list[str] | None, everything: bool) -> list[dict]:
    if everything:
        return docs
    if groups:
        return [d for d in docs if d["group"] in groups]
    return [d for d in docs if d.get("core")]


def fetch(doc: dict, dest: Path, verify_only: bool) -> str:
    target = dest / doc["local"]
    if target.exists() and sha256(target) == doc["sha256"]:
        return "ok"
    if verify_only:
        return "missing" if not target.exists() else "MISMATCH"
    if "archive" in doc:
        arc = doc["archive"]
        apath = dest / "archives" / arc["file"]
        if not (apath.exists() and sha256(apath) == arc["sha256"]):
            download(arc["url"], apath)
            if sha256(apath) != arc["sha256"]:
                apath.unlink()
                return "MISMATCH (archive)"
        with zipfile.ZipFile(apath) as z:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(z.read(arc["member"]))
    else:
        download(doc["url"], target)
    if sha256(target) != doc["sha256"]:
        target.unlink()
        return "MISMATCH"
    return "downloaded"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--group", action="append")
    ap.add_argument("--id", action="append", help="one document id; may be repeated")
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--dest", type=Path, default=DEST)
    a = ap.parse_args()
    docs = select(json.loads(MANIFEST.read_text())["documents"], a.group, a.all or bool(a.id))
    if a.id:
        docs = [d for d in docs if d["id"] in a.id]
    failures = 0
    counts: dict[str, int] = {}
    for d in docs:
        try:
            status = fetch(d, a.dest, a.verify)
        except RuntimeError as e:
            status = f"FAILED ({e})"
        counts[status.split()[0]] = counts.get(status.split()[0], 0) + 1
        if status not in ("ok", "downloaded"):
            failures += 1
            print(f"{d['id']}: {status}", file=sys.stderr)
    print(f"{len(docs)} document(s): " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
