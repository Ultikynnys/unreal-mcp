#!/usr/bin/env python3
"""Fail when a GitHub workflow pins nothing.

Two kinds of CI breakage arrive without a commit to this repo:

  * ``runs-on: ubuntu-latest`` - the label migrates (ubuntu-latest moved 24.04 -> 26 on
    2026-10-19), so the runner under the job changes on someone else's schedule;
  * ``uses: some/action@main`` - a moving branch, so the code that runs changes under you.

Both are fixed by pinning: a concrete image (``ubuntu-24.04``) and a release tag or commit SHA
(``@v7``, ``@v10.2.0``, ``@<sha>``). This scan is a line-level check, so it needs no YAML
dependency; a value containing an expression (``${{ ... }}``) cannot be judged statically and is
reported as INFO rather than guessed at.

    python Python/scripts/check_workflow_pins.py
    python Python/scripts/check_workflow_pins.py --root <dir>
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

DEFAULT_ROOT = pathlib.Path(__file__).resolve().parents[2]
# pathlib.glob has no brace expansion, so the extensions are listed separately.
WORKFLOW_PATTERNS = (".github/workflows/*.yml", ".github/workflows/*.yaml")

RUNS_ON_RE = re.compile(r"^\s*runs-on:\s*(.+?)\s*(?:#.*)?$", re.MULTILINE)
USES_RE = re.compile(r"^\s*-?\s*uses:\s*(.+?)\s*(?:#.*)?$", re.MULTILINE)

# Moving refs. A tag or SHA is fine; these are not.
BRANCH_REFS = {"main", "master", "head", "trunk", "develop", "latest"}


def clean(value: str) -> str:
    """Strip surrounding quotes and whitespace from a YAML scalar."""
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        value = value[1:-1]
    return value


def workflow_findings(text: str) -> list[str]:
    """Human-readable problems with one workflow file (empty when it is pinned)."""
    findings: list[str] = []
    for match in RUNS_ON_RE.finditer(text):
        value = clean(match.group(1))
        if "${{" in value:
            continue
        if value.endswith("-latest"):
            findings.append(f"floating runner label: runs-on: {value} "
                            f"(pin the image, e.g. ubuntu-24.04)")
    for match in USES_RE.finditer(text):
        value = clean(match.group(1))
        if "${{" in value or value.startswith(("./", "docker://")):
            continue
        if "@" not in value:
            findings.append(f"unpinned action: uses: {value} (add @vN or a commit SHA)")
            continue
        ref = value.split("@", 1)[1]
        if ref.lower() in BRANCH_REFS or ref.startswith("refs/heads/"):
            findings.append(f"branch ref: uses: {value} (a moving branch; pin @vN or a SHA)")
    return findings


def scan_file(path: pathlib.Path) -> list[str]:
    return [f"  {path}: {finding}" for finding in workflow_findings(
        path.read_text(encoding="utf-8", errors="replace"))]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=str(DEFAULT_ROOT))
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)

    root = pathlib.Path(args.root).resolve()
    files = sorted({path for pattern in WORKFLOW_PATTERNS for path in root.glob(pattern)})
    findings: list[str] = []
    for path in files:
        findings.extend(scan_file(path))

    if not args.quiet:
        print(f"Scanned {len(files)} workflow file(s) for floating runners and action refs")

    if findings:
        print(f"FAIL: {len(findings)} unpinned workflow setting(s) "
              f"(these change CI without a commit here):")
        print("\n".join(findings))
        return 1

    if not args.quiet:
        print("OK: every workflow pins its runner image and its action versions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
