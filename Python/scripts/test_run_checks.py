#!/usr/bin/env python3
"""Unit tests for run_checks.py - the battery behind the pre-push hook.

The hook's promise is "a failing check blocks the push", so the property worth pinning is
that summarize() fails closed: any non-zero return code, from any check, yields a non-zero
exit. The real tree is checked by the other suites; these tests use fixtures so they stay
fast and editor-free.
"""

from __future__ import annotations

import importlib.util
import pathlib
import unittest

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("run_checks", SCRIPT_DIR / "run_checks.py")
run_checks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run_checks)


class SummarizeTests(unittest.TestCase):
    def test_all_green_exits_zero(self):
        code, summary = run_checks.summarize([("a", 0), ("b", 0)])
        self.assertEqual(code, 0)
        self.assertEqual(summary, "2/2 checks passed")

    def test_any_failure_exits_non_zero_and_names_the_check(self):
        code, summary = run_checks.summarize([("a", 0), ("b", 1), ("c", 0)])
        self.assertEqual(code, 1)
        self.assertIn("2/3 checks passed", summary)
        self.assertIn("b", summary)

    def test_non_one_failure_codes_also_fail(self):
        """127 (missing script) and 124 (timeout) must not slip through."""
        for bad in (2, 124, 127, -9):
            code, _ = run_checks.summarize([("a", 0), ("b", bad)])
            self.assertEqual(code, 1, f"return code {bad} was treated as success")

    def test_empty_run_is_not_a_failure(self):
        code, summary = run_checks.summarize([])
        self.assertEqual(code, 0)
        self.assertEqual(summary, "0/0 checks passed")


class CheckListTests(unittest.TestCase):
    def test_every_offline_check_script_exists(self):
        for _label, script, _extra in run_checks.CHECKS:
            self.assertTrue((SCRIPT_DIR / script).is_file(), f"missing check script: {script}")

    def test_live_checks_are_not_in_the_offline_battery(self):
        """A push must not need a running editor."""
        offline = {script for _l, script, _e in run_checks.CHECKS}
        for _label, script, _extra in run_checks.LIVE_CHECKS:
            self.assertNotIn(script, offline)

    def test_missing_script_fails_closed(self):
        _label, code, _seconds, tail = run_checks.run_check("ghost", "does_not_exist.py")
        self.assertEqual(code, 127)
        self.assertIn("missing check script", tail)


if __name__ == "__main__":
    unittest.main(verbosity=2)
