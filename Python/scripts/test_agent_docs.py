#!/usr/bin/env python3
"""Contract test for the agent-facing tool documentation.

What the agent knows about this server comes from three generated surfaces: the
FastMCP `instructions`, the `info` prompt, and the per-tool JSON schemas. The first
two are rendered from Python/tool_catalog.py, and this test locks the properties
that make them trustworthy:

  1. discovery is decorator-based, so a commented-out tool (or a register_* wrapper)
     can never be documented as if it were callable,
  2. the reference names every tool and no tool that does not exist,
  3. the instructions carry the operational facts an agent needs (the control-plane
     rule, the failure codes, the lifecycle recovery path),
  4. both stay inside a size budget, because the instructions are injected by the
     client on every session.

Run directly or with unittest.
"""

from __future__ import annotations

import importlib.util
import pathlib
import re
import unittest

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[1]
TOOLS_DIR = REPO_ROOT / "Python" / "tools"

spec = importlib.util.spec_from_file_location("tool_catalog", REPO_ROOT / "Python" / "tool_catalog.py")
tool_catalog = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool_catalog)

# A tool named in the reference is `name(params)`; capture the name.
MENTION_RE = re.compile(r"`([a-z_][a-z0-9_]*)\(([^`]*)\)`")

# Budgets. The instructions are injected once per session by the client; the reference
# is fetched on demand. Generous but bounded, so neither can quietly become a wall.
MAX_INSTRUCTIONS_CHARS = 2000
MAX_REFERENCE_CHARS = 20000


class DiscoveryTests(unittest.TestCase):
    def test_discovers_the_registered_surface(self):
        catalog = tool_catalog.discover_tools(TOOLS_DIR)
        self.assertEqual(len(catalog), 78, "the tool count changed; update this expectation")
        self.assertEqual(len({t.name for t in catalog}), len(catalog), "duplicate tool names")

    def test_commented_out_tools_are_not_documented(self):
        """The old prompt named focus_viewport; it is commented out, so it is not a tool."""
        names = {t.name for t in tool_catalog.discover_tools(TOOLS_DIR)}
        self.assertNotIn("focus_viewport", names)
        self.assertNotIn("set_pawn_properties", names)

    def test_every_tool_has_a_summary_and_a_command(self):
        for tool in tool_catalog.discover_tools(TOOLS_DIR):
            self.assertTrue(tool.summary, f"{tool.name} has no docstring first line")
            self.assertTrue(tool.command, f"{tool.name} sends no command")


class ReferenceTests(unittest.TestCase):
    def setUp(self):
        self.catalog = tool_catalog.discover_tools(TOOLS_DIR)
        self.reference = tool_catalog.render_tool_reference(self.catalog)

    def test_names_every_registered_tool(self):
        mentioned = {m.group(1) for m in MENTION_RE.finditer(self.reference)}
        missing = {t.name for t in self.catalog} - mentioned
        self.assertEqual(missing, set(), f"tools absent from the reference: {sorted(missing)}")

    def test_names_no_phantom_tool(self):
        mentioned = {m.group(1) for m in MENTION_RE.finditer(self.reference)}
        known = {t.name for t in self.catalog}
        phantoms = mentioned - known
        self.assertEqual(phantoms, set(), f"reference names non-existent tools: {sorted(phantoms)}")

    def test_parameters_come_from_the_tool(self):
        """Guards the class of bug the old prompt had: spawn_actor(..., scale) did not exist."""
        line = next(l for l in self.reference.splitlines() if l.startswith("- `spawn_actor("))
        params = line.split("(", 1)[1].split(")", 1)[0]
        self.assertNotIn("scale", params)
        self.assertIn("allow_duplicate", params)

    def test_reference_within_budget(self):
        self.assertLess(len(self.reference), MAX_REFERENCE_CHARS)


class InstructionsTests(unittest.TestCase):
    def setUp(self):
        self.catalog = tool_catalog.discover_tools(TOOLS_DIR)
        self.text = tool_catalog.render_instructions(self.catalog)

    def test_carries_the_operational_facts(self):
        self.assertIn("127.0.0.1:55557", self.text)
        self.assertIn("EDITOR_MODAL_ACTIVE", self.text)
        self.assertIn("EDITOR_BUSY", self.text)
        self.assertIn("recover_editor", self.text)
        self.assertIn("editor_process.py", self.text)
        self.assertIn("bridge_state.json", self.text)

    def test_states_the_tool_count_and_points_at_the_prompt(self):
        self.assertIn(f"({len(self.catalog)} tools)", self.text)
        self.assertIn("`info` prompt", self.text)

    def test_instructions_within_budget(self):
        self.assertLess(len(self.text), MAX_INSTRUCTIONS_CHARS)


if __name__ == "__main__":
    unittest.main(verbosity=2)
