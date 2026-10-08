#!/usr/bin/env python3
"""Contract test for Docs/: the prose must not document tools that do not exist.

The markdown under Docs/Tools is hand-written next to the same surface the agent prompt
was, and it drifted the same way: it documented `focus_viewport`, a commented-out tool,
with parameters, a JSON example and a Python snippet. This test ties the prose to the
catalog:

  1. the generated reference (Docs/Tools/REFERENCE.md) matches the tools right now,
  2. it names every tool and no tool that is not registered,
  3. no markdown under Docs/ cites an unregistered tool, across the four ways these docs
     cite one: `"command": "x"` in JSON examples, `send_command("x")` in Python examples,
     `### x` snake_case headings, and `` `x(...)` `` inline code.

Identifiers that are not tools (a connection helper in a code sample, a sibling page name)
are allowlisted explicitly, and the checker is proven to fail on a synthetic phantom.

Run directly or with unittest.
"""

from __future__ import annotations

import importlib.util
import pathlib
import re
import sys
import tempfile
import textwrap
import unittest

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[1]
DOCS_DIR = REPO_ROOT / "Docs"
TOOLS_DIR = REPO_ROOT / "Python" / "tools"

sys.path.insert(0, str(REPO_ROOT / "Python"))
import tool_catalog  # noqa: E402

spec = importlib.util.spec_from_file_location("gen_tool_docs", SCRIPT_DIR / "gen_tool_docs.py")
gen_tool_docs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen_tool_docs)

# Identifiers that legitimately appear in these docs but are not tool names.
NON_TOOL_IDENTIFIERS = {
    "get_unreal_connection",   # server helper used in Python samples
    "send_command",            # connection method used in Python samples
    "actor_tools", "editor_tools", "asset_tools", "blueprint_tools", "node_tools",
    "gen_tool_docs",           # scripts referenced by path
    "editor_process",
    "unreal_mcp_server",
}

CITATION_PATTERNS = {
    # {"command": "spawn_actor"}
    "json example": re.compile(r'"command"\s*:\s*"([a-z_][a-z0-9_]*)"'),
    # unreal.send_command("spawn_actor", ...)
    "python example": re.compile(r'send_command\(\s*"([a-z_][a-z0-9_]*)"'),
    # ### spawn_actor
    "section heading": re.compile(r"^#+\s+([a-z_][a-z0-9_]*)\s*$", re.MULTILINE),
    # `spawn_actor(...)`
    "inline code": re.compile(r"`([a-z_][a-z0-9_]*)\([^`]\)?`"),
}


def _load_catalog():
    return tool_catalog.discover_tools(TOOLS_DIR)


def find_phantom_citations(markdown_by_path: dict[str, str], known_tools: set[str],
                           known_commands: set[str]) -> dict[str, list[str]]:
    """{path: ["<how>: <name>"]} for every citation of a tool that is not registered."""
    findings: dict[str, list[str]] = {}
    for path, text in sorted(markdown_by_path.items()):
        for how, pattern in CITATION_PATTERNS.items():
            for name in pattern.findall(text):
                if name in NON_TOOL_IDENTIFIERS:
                    continue
                pool = known_commands if how == "json example" else known_tools
                if name not in pool:
                    findings.setdefault(path, []).append(f"{how}: {name}")
    return findings


def _markdown_docs() -> dict[str, str]:
    return {str(p): p.read_text(encoding="utf-8") for p in sorted(DOCS_DIR.rglob("*.md"))}


class ReferenceFreshnessTests(unittest.TestCase):
    def test_generated_reference_is_current(self):
        catalog = _load_catalog()
        expected = gen_tool_docs.render_reference_doc(catalog)
        actual = gen_tool_docs.DOC_PATH.read_text(encoding="utf-8")
        self.assertEqual(
            actual, expected,
            "Docs/Tools/REFERENCE.md is stale - run Python/scripts/gen_tool_docs.py")

    def test_reference_names_every_tool_and_no_phantom(self):
        text = gen_tool_docs.DOC_PATH.read_text(encoding="utf-8")
        named = set(re.findall(r"^\| `([a-z_][a-z0-9_]*)` \|", text, re.MULTILINE))
        known = {t.name for t in _load_catalog()}
        self.assertEqual(known - named, set(), "missing from REFERENCE.md")
        self.assertEqual(named - known, set(), "REFERENCE.md names non-existent tools")


class ProseCitationTests(unittest.TestCase):
    def setUp(self):
        catalog = _load_catalog()
        self.known_tools = {t.name for t in catalog}
        self.known_commands = {t.command for t in catalog} | {"ping", "reload_server"}

    def test_docs_cite_no_unregistered_tool(self):
        findings = find_phantom_citations(_markdown_docs(), self.known_tools, self.known_commands)
        self.assertEqual(findings, {}, f"docs cite tools that do not exist: {findings}")

    def test_detects_a_phantom_citation(self):
        """Guard the guard: the checker must fail on a page describing a removed tool."""
        with tempfile.TemporaryDirectory() as d:
            page = pathlib.Path(d) / "bad_tools.md"
            page.write_text(textwrap.dedent('''
                # Bad page

                ### focus_viewport

                ```json
                { "command": "focus_viewport", "params": { "target": "PlayerStart" } }
                ```

                Call `focus_viewport(location)` after connecting.
            '''), encoding="utf-8")
            findings = find_phantom_citations(
                {str(page): page.read_text(encoding="utf-8")},
                self.known_tools, self.known_commands)
            self.assertIn(str(page), findings)
            self.assertTrue(any("focus_viewport" in f for f in findings[str(page)]))


if __name__ == "__main__":
    unittest.main(verbosity=2)
