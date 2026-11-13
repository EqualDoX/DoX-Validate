"""Shared fixtures of the dox-validate black-box test suite (see tests/README.md).

The binary comes from $DOX_VALIDATE (default: build/dox-validate). Every test runs offline; tests that need the downloaded
public documents or the generated size tiers skip when tests/corpus/.cache does not hold them.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
CORPUS = ROOT / "tests" / "corpus"
FIXTURES = CORPUS / "fixtures"
GENERATED = CORPUS / ".cache" / "generated"
PUBLIC = CORPUS / ".cache" / "public"
SCHEMA = ROOT / "schema" / "eqx-validator-output-1.schema.json"
sys.path.insert(0, str(CORPUS))

BINARY = Path(os.environ.get("DOX_VALIDATE", ROOT / "build" / "dox-validate"))
if not BINARY.is_absolute():
    BINARY = ROOT / BINARY


@dataclass
class Run:
    code: int
    stdout: bytes
    stderr: str

    def json(self) -> dict:
        return json.loads(self.stdout)

    def json_lines(self) -> list[dict]:
        return [json.loads(line) for line in self.stdout.splitlines() if line.strip()]


def run(*args: str | Path, timeout: float = 120) -> Run:
    p = subprocess.run([str(BINARY), *map(str, args)], capture_output=True, timeout=timeout)
    return Run(p.returncode, p.stdout, p.stderr.decode("utf-8", "replace"))


@pytest.fixture(scope="session", autouse=True)
def _binary_present():
    if not BINARY.exists():
        pytest.exit(f"dox-validate binary not found at {BINARY}; build it or set DOX_VALIDATE", returncode=4)


def load_expected() -> list[dict]:
    return json.loads((FIXTURES / "expected.json").read_text())["fixtures"]


def load_generated() -> list[dict]:
    p = GENERATED / "generated.json"
    return json.loads(p.read_text())["fixtures"] if p.exists() else []


def load_public() -> list[dict]:
    return json.loads((CORPUS / "public" / "manifest.json").read_text())["documents"]


@lru_cache(maxsize=1)
def catalogue() -> dict[str, tuple[str, str]]:
    """rule id -> (severity, module) from --list-rules."""
    out = run("--list-rules")
    assert out.code == 0
    rules = {}
    for line in out.stdout.decode().splitlines():
        rid, sev, module, _title = line.split("\t", 3)
        rules[rid] = (sev, module)
    return rules


@lru_cache(maxsize=2)
def fixture_reports(target: str = "") -> dict[str, tuple[dict, int]]:
    """Every committed fixture validated once (one process per file): file name -> (report, exit status).
    target: "" = the default result basis (auto), else the --target value."""
    reports = {}
    extra = ["--target", target] if target else []
    for spec in load_expected():
        r = run("--format", "json", *extra, FIXTURES / spec["file"])
        reports[spec["file"]] = (r.json(), r.code)
    return reports


@pytest.fixture(scope="session")
def reports():
    return fixture_reports()


@pytest.fixture(scope="session")
def reports_any():
    return fixture_reports("any")


@pytest.fixture(scope="session")
def schema_validator():
    import jsonschema
    schema = json.loads(SCHEMA.read_text())
    jsonschema.Draft202012Validator.check_schema(schema)
    return jsonschema.Draft202012Validator(schema, format_checker=jsonschema.FormatChecker())


def strip_timing(report: dict) -> dict:
    r = dict(report)
    r.pop("timing_ms", None)
    return r


def known_defects() -> dict:
    p = CORPUS / "known_defects.json"
    return json.loads(p.read_text())["defects"] if p.exists() else {}
