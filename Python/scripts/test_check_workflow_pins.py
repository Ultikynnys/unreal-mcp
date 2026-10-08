#!/usr/bin/env python3
"""Unit tests for check_workflow_pins.py.

CI broke twice on things this scan is meant to catch: an action targeting a deprecated runtime
(which is why the ref must be a real version, not a branch) and a runner label that migrates
under the job (which is why the image must be concrete). The fixtures are the shapes that appear
in this repo's own workflow.
"""

from __future__ import annotations

import importlib.util
import pathlib
import tempfile
import unittest

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("check_workflow_pins",
                                              SCRIPT_DIR / "check_workflow_pins.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)

WORKFLOW_REL = ".github/workflows/example.yml"


class FindingTests(unittest.TestCase):
    def test_pinned_workflow_is_clean(self):
        text = ("jobs:\n"
                "  build:\n"
                "    runs-on: ubuntu-24.04\n"
                "    steps:\n"
                "      - uses: actions/checkout@v7\n"
                "      - uses: astral-sh/setup-uv@v10.2.0\n")
        self.assertEqual(checker.workflow_findings(text), [])

    def test_floating_runner_label_is_flagged(self):
        findings = checker.workflow_findings("    runs-on: ubuntu-latest\n")
        self.assertEqual(len(findings), 1)
        self.assertIn("ubuntu-latest", findings[0])

    def test_windows_runner_latest_is_flagged(self):
        self.assertTrue(checker.workflow_findings("    runs-on: windows-latest\n"))

    def test_branch_ref_is_flagged(self):
        findings = checker.workflow_findings("      - uses: actions/checkout@main\n")
        self.assertEqual(len(findings), 1)
        self.assertIn("branch ref", findings[0])

    def test_refs_heads_form_is_flagged(self):
        self.assertTrue(checker.workflow_findings("      - uses: a/b@refs/heads/main\n"))

    def test_unpinned_action_is_flagged(self):
        findings = checker.workflow_findings("      - uses: actions/checkout\n")
        self.assertEqual(len(findings), 1)
        self.assertIn("unpinned action", findings[0])

    def test_commit_sha_and_concrete_tag_are_allowed(self):
        text = "      - uses: a/b@v1\n      - uses: a/b@v1.2.3\n      - uses: a/b@8f4b7f8\n"
        self.assertEqual(checker.workflow_findings(text), [])

    def test_matrix_expression_is_not_guessed_at(self):
        self.assertEqual(checker.workflow_findings("    runs-on: ${{ matrix.os }}\n"), [])

    def test_local_and_docker_actions_are_out_of_scope(self):
        text = "      - uses: ./.github/actions/x\n      - uses: docker://alpine:3\n"
        self.assertEqual(checker.workflow_findings(text), [])

    def test_quoted_and_commented_values(self):
        self.assertTrue(checker.workflow_findings('    runs-on: "ubuntu-latest"  # pin me\n'))
        self.assertEqual(checker.workflow_findings('    runs-on: "ubuntu-24.04"\n'), [])


class CliTests(unittest.TestCase):
    def _write(self, root: pathlib.Path, body: str) -> None:
        path = root / WORKFLOW_REL
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body, encoding="utf-8")

    def test_real_repo_is_pinned(self):
        repo_root = SCRIPT_DIR.parents[1]
        self.assertEqual(checker.main(["--root", str(repo_root), "--quiet"]), 0)

    def test_clean_tree_exits_zero(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            self._write(tmp, "    runs-on: ubuntu-24.04\n      - uses: a/b@v1\n")
            self.assertEqual(checker.main(["--root", str(tmp), "--quiet"]), 0)

    def test_floating_label_exits_one(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            self._write(tmp, "    runs-on: ubuntu-latest\n")
            self.assertEqual(checker.main(["--root", str(tmp), "--quiet"]), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
