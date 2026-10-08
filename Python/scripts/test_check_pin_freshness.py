#!/usr/bin/env python3
"""Unit tests for check_pin_freshness.py.

The point of the ledger is that an aged pin stops being silent, so the tests are about the
decisions: overdue fails, approaching warns, and the ledger cannot rot in either direction (a
workflow pin that is unregistered fails, a registered pin that vanished fails). Every date is
injected, so the tests do not drift toward the real deadlines.
"""

from __future__ import annotations

import datetime
import importlib.util
import json
import pathlib
import tempfile
import unittest

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("check_pin_freshness",
                                              SCRIPT_DIR / "check_pin_freshness.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)

WORKFLOW_REL = ".github/workflows/tool-parity.yml"
WORKFLOW = ("jobs:\n"
            "  build:\n"
            "    runs-on: ubuntu-24.04\n"
            "    steps:\n"
            "      - uses: actions/checkout@v7\n"
            "      - uses: astral-sh/setup-uv@v10.2.0\n"
            "        with:\n"
            '          version: "0.12.18"\n'
            '          python-version: "3.12"\n')


def day(year: int, month: int, day_of_month: int) -> datetime.date:
    return datetime.date(year, month, day_of_month)


def ledger(review_by: str = "2027-01-15") -> dict:
    """A ledger matching WORKFLOW."""
    return {"pins": [
        {"where": WORKFLOW_REL, "kind": "runs-on", "value": "ubuntu-24.04",
         "review_by": review_by},
        {"where": WORKFLOW_REL, "kind": "uses", "value": "actions/checkout@v7",
         "review_by": review_by},
        {"where": WORKFLOW_REL, "kind": "uses", "value": "astral-sh/setup-uv@v10.2.0",
         "review_by": review_by},
        {"where": WORKFLOW_REL, "kind": "version", "value": "0.12.18", "review_by": review_by},
    ]}


def found_from_workflow() -> list[tuple[str, str, str]]:
    return [(WORKFLOW_REL, kind, value)
            for kind, value in checker.accountable_pins(WORKFLOW)]


class LedgerTests(unittest.TestCase):
    def test_matching_ledger_and_future_date_pass(self):
        failures, warnings = checker.ledger_findings(
            ledger(), found_from_workflow(), day(2026, 10, 8))
        self.assertEqual(failures, [])
        self.assertEqual(warnings, [])

    def test_overdue_review_fails_and_says_by_how_much(self):
        failures, _ = checker.ledger_findings(ledger(), found_from_workflow(), day(2027, 2, 1))
        self.assertEqual(len(failures), 4)
        self.assertIn("overdue by 17 day(s)", failures[0])

    def test_approaching_review_only_warns(self):
        failures, warnings = checker.ledger_findings(
            ledger(), found_from_workflow(), day(2027, 1, 5))
        self.assertEqual(failures, [])
        self.assertEqual(len(warnings), 4)
        self.assertIn("due in 10 day(s)", warnings[0])

    def test_unregistered_workflow_pin_fails(self):
        found = found_from_workflow() + [(WORKFLOW_REL, "uses", "some/action@v1")]
        failures, _ = checker.ledger_findings(ledger(), found, day(2026, 10, 8))
        self.assertEqual(len(failures), 1)
        self.assertIn("unregistered pin", failures[0])

    def test_registered_pin_that_vanished_fails(self):
        found = [item for item in found_from_workflow() if item[2] != "actions/checkout@v7"]
        failures, _ = checker.ledger_findings(ledger(), found, day(2026, 10, 8))
        self.assertEqual(len(failures), 1)
        self.assertIn("no longer in", failures[0])

    def test_invalid_review_date_fails(self):
        bad = ledger()
        bad["pins"][0]["review_by"] = "soon"
        failures, _ = checker.ledger_findings(bad, found_from_workflow(), day(2026, 10, 8))
        self.assertEqual(len(failures), 1)
        self.assertIn("invalid review_by", failures[0])


class ExtractionTests(unittest.TestCase):
    def test_extracts_every_shelf_life_value(self):
        kinds = [kind for kind, _value in checker.check_workflow_pins.extract_pins(WORKFLOW)]
        self.assertEqual(kinds, ["runs-on", "uses", "uses", "version"])

    def test_python_version_is_not_mistaken_for_version(self):
        self.assertNotIn("python-version", str(checker.accountable_pins(WORKFLOW)))
        self.assertIn(("version", "0.12.18"), checker.accountable_pins(WORKFLOW))

    def test_expressions_and_local_refs_are_not_accounted_for(self):
        text = ("    runs-on: ${{ matrix.os }}\n"
                "      - uses: ./.github/actions/local\n"
                "      - uses: docker://alpine:3\n")
        self.assertEqual(checker.accountable_pins(text), [])


class CliTests(unittest.TestCase):
    def test_registered_and_future_exits_zero(self):
        with tempfile.TemporaryDirectory() as d:
            self._tree(d, WORKFLOW, ledger())
            code = checker.main(["--root", d, "--quiet", "--today", "2026-10-08"])
        self.assertEqual(code, 0)

    def test_overdue_exits_one(self):
        with tempfile.TemporaryDirectory() as d:
            self._tree(d, WORKFLOW, ledger())
            code = checker.main(["--root", d, "--quiet", "--today", "2027-03-01"])
        self.assertEqual(code, 1)

    def test_missing_ledger_exits_one(self):
        with tempfile.TemporaryDirectory() as d:
            root = pathlib.Path(d)
            workflow = root / WORKFLOW_REL
            workflow.parent.mkdir(parents=True, exist_ok=True)
            workflow.write_text(WORKFLOW, encoding="utf-8")
            code = checker.main(["--root", d, "--quiet"])
        self.assertEqual(code, 1)

    def test_real_repo_ledger_is_current(self):
        repo_root = SCRIPT_DIR.parents[1]
        self.assertEqual(checker.main(["--root", str(repo_root), "--quiet"]), 0)

    @staticmethod
    def _tree(directory: str, body: str, pins: dict) -> None:
        root = pathlib.Path(directory)
        workflow = root / WORKFLOW_REL
        workflow.parent.mkdir(parents=True, exist_ok=True)
        workflow.write_text(body, encoding="utf-8")
        (root / checker.LEDGER_REL).write_text(json.dumps(pins), encoding="utf-8")


if __name__ == "__main__":
    unittest.main(verbosity=2)
