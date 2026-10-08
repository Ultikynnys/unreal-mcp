#!/usr/bin/env python3
"""Fail when a CI pin has passed its review date, or when the ledger and the workflows disagree.

A pinned value is a promise with a shelf life: GitHub retires runner images, deprecates action
runtimes, and releases new major versions on its own schedule, none of which arrives as a commit
here. ``.github/pins.json`` records each pin with the date it must be looked at again, so an aged
pin becomes a failing build (and the weekly CI run surfaces it even when nobody pushes) instead
of a surprise.

The ledger is checked against the workflows in both directions, so it cannot rot either way:
a workflow pin that is not registered fails, and a registered pin that no longer exists fails.
Within the warning window the check only prints a note.

    python Python/scripts/check_pin_freshness.py
    python Python/scripts/check_pin_freshness.py --today 2027-02-01   # what a given day would say
"""

from __future__ import annotations

import argparse
import datetime as dt
import importlib.util
import json
import pathlib

DEFAULT_ROOT = pathlib.Path(__file__).resolve().parents[2]
LEDGER_REL = ".github/pins.json"
WORKFLOW_PATTERNS = (".github/workflows/*.yml", ".github/workflows/*.yaml")
WARN_DAYS = 14

# Reuse the one pin extractor rather than re-parsing workflows in a second place.
_pins_spec = importlib.util.spec_from_file_location(
    "check_workflow_pins", pathlib.Path(__file__).resolve().parent / "check_workflow_pins.py")
check_workflow_pins = importlib.util.module_from_spec(_pins_spec)
_pins_spec.loader.exec_module(check_workflow_pins)


def ledger_findings(ledger: dict, found: list[tuple[str, str, str]],
                    today: dt.date) -> tuple[list[str], list[str]]:
    """(failures, warnings) comparing the ledger with the pins present in the workflows.

    found is [(where, kind, value)] for every pinned value that is not an expression or a local
    reference. A pin that the checker cannot resolve statically is not accounted for.
    """
    failures: list[str] = []
    warnings: list[str] = []
    present = {(where, kind, value) for where, kind, value in found}
    registered = {}
    for entry in ledger.get("pins", []):
        key = (entry.get("where"), entry.get("kind"), entry.get("value"))
        registered[key] = entry
        if key not in present:
            failures.append(f"ledger entry no longer in {key[0]}: {key[1]}: {key[2]} "
                            f"(the pin changed or moved; update {LEDGER_REL})")

    for where, kind, value in sorted(present):
        if (where, kind, value) not in registered:
            failures.append(f"unregistered pin: {where} {kind}: {value} "
                            f"(add it to {LEDGER_REL} with a review date)")
            continue
        raw = registered[(where, kind, value)].get("review_by")
        try:
            review_by = dt.date.fromisoformat(str(raw))
        except ValueError:
            failures.append(f"invalid review_by for {kind}: {value}: {raw!r} (want YYYY-MM-DD)")
            continue
        overdue = (today - review_by).days
        if overdue > 0:
            failures.append(f"review overdue by {overdue} day(s): {kind}: {value} "
                            f"(was due {review_by}; check the source, then bump review_by)")
        elif -overdue <= WARN_DAYS:
            warnings.append(f"review due in {-overdue} day(s): {kind}: {value} (due {review_by})")
    return failures, warnings


def accountable_pins(text: str) -> list[tuple[str, str]]:
    """extract_pins minus what cannot be judged statically (expressions, local/docker refs)."""
    return [(kind, value) for kind, value in check_workflow_pins.extract_pins(text)
            if "${{" not in value and not value.startswith(("./", "docker://"))]


def collect_pins(root: pathlib.Path) -> list[tuple[str, str, str]]:
    """[(where, kind, value)] for every statically resolvable pin in every workflow."""
    found = []
    for pattern in WORKFLOW_PATTERNS:
        for path in sorted(root.glob(pattern)):
            text = path.read_text(encoding="utf-8", errors="replace")
            where = str(path.relative_to(root)).replace("\\", "/")
            for kind, value in accountable_pins(text):
                found.append((where, kind, value))
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=str(DEFAULT_ROOT))
    parser.add_argument("--today", default=None, help="YYYY-MM-DD, for testing what a day says")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)

    root = pathlib.Path(args.root).resolve()
    today = dt.date.fromisoformat(args.today) if args.today else dt.date.today()

    ledger_path = root / LEDGER_REL
    if not ledger_path.is_file():
        print(f"FAIL: no pin ledger at {LEDGER_REL}; every pinned CI value must be registered "
              f"with a review date")
        return 1
    ledger = json.loads(ledger_path.read_text(encoding="utf-8"))

    found = collect_pins(root)
    failures, warnings = ledger_findings(ledger, found, today)

    if not args.quiet:
        print(f"Scanned {len(found)} CI pin(s) against {LEDGER_REL} (today {today})")

    if failures:
        print(f"FAIL: {len(failures)} pin(s) need attention:")
        print("\n".join(f"  {item}" for item in failures))
        return 1

    for item in warnings:
        print(f"note: {item}")
    if not args.quiet:
        print("OK: every pin is registered and its review date is still ahead")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
