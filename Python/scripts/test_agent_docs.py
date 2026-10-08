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


class HygieneTests(unittest.TestCase):
    """Everything an agent may read must be true: no ghost fixtures, no phantom parameters,
    and no instruction naming a file that does not exist.

    Each of these catches a bug that was live in this repo:
      - smoke_all_tools.py carried a parameter map entry for find_actors_by_name, a tool that
        no longer existed;
      - spawn_actor's Args block omitted allow_duplicate, so the option was invisible;
      - the control-plane banner (and the C++ refusal text) told the agent not to copy
        Python/editor/archive_mcp_client.py, which had already been deleted.
    """

    @staticmethod
    def _args_entries(docstring: str) -> set[str]:
        """Parameter names documented in the Args: section (Returns: fields are not params)."""
        if "Args:" not in docstring:
            return set()
        section = docstring.split("Args:", 1)[1]
        for stop in ("Returns:", "Note:", "Example:", "Raises:"):
            section = section.split(stop, 1)[0]
        names: set[str] = set()
        for match in re.finditer(r"^\s+([a-z_][a-z0-9_/]*)\s*:", section, re.MULTILINE):
            names |= set(match.group(1).split("/"))
        return names - {"ctx"}  # the MCP context is internal, not a tool parameter

    @staticmethod
    def _smoke_text() -> str:
        return (REPO_ROOT / "Python" / "scripts" / "smoke_all_tools.py").read_text(encoding="utf-8")

    def test_smoke_parameters_reference_only_real_tools(self):
        text = self._smoke_text()
        block = text[text.index("PARAMS = {"):text.index("UNKNOWN_MARKERS")]
        keys = set(re.findall(r'^\s{4}"([a-z_][a-z0-9_]+)":', block, re.MULTILINE))
        known = {t.name for t in tool_catalog.discover_tools(TOOLS_DIR)}
        skip = set(re.findall(r'"([a-z_][a-z0-9_]+)"',
                              text[text.index("SKIP = {"):text.index("# Removes everything")]))
        ghosts = keys - known - skip
        self.assertEqual(ghosts, set(), f"smoke parameter map names non-existent tools: {sorted(ghosts)}")

    def test_every_tool_is_exercised_by_the_smoke(self):
        """A tool that no check calls is a tool nobody noticed was broken.

        Every registered tool must be reachable by the smoke script: it has a PARAMS entry, it
        is listed in SKIP with a reason, or it takes no parameters (so the no-argument call is
        already the exercise). A new tool with parameters and no entry fails here.
        """
        text = self._smoke_text()
        block = text[text.index("PARAMS = {"):text.index("UNKNOWN_MARKERS")]
        covered = set(re.findall(r'^\s{4}"([a-z_][a-z0-9_]+)":', block, re.MULTILINE))
        covered |= set(re.findall(r'"([a-z_][a-z0-9_]+)"',
                                  text[text.index("SKIP = {"):text.index("# Removes everything")]))
        uncovered = sorted(t.name for t in tool_catalog.discover_tools(TOOLS_DIR)
                           if t.name not in covered and t.params)
        self.assertEqual(uncovered, [],
                         f"tools with parameters and no smoke coverage: {uncovered}")

    def test_prepush_hook_runs_the_battery_and_cannot_be_silenced(self):
        """The gate itself must be wired: a hook that skips or swallows failures is worse
        than no hook, because it looks like a check."""
        hook = REPO_ROOT / ".githooks" / "pre-push"
        self.assertTrue(hook.is_file(), "the tracked pre-push hook is missing")
        text = hook.read_text(encoding="utf-8")
        self.assertIn("run_checks.py", text, "the hook does not run the shared battery")
        for silencing in ("|| true", "|| :", "--no-verify", "exit 0"):
            self.assertNotIn(silencing, text, f"the hook can be silenced with '{silencing}'")
        runner = (REPO_ROOT / "Python" / "scripts" / "run_checks.py").read_text(encoding="utf-8")
        self.assertIn('"Python <-> C++ parity"', runner,
                      "the parity checker is not part of the battery the hook runs")

    def test_docstrings_document_only_real_parameters(self):
        import ast
        problems: dict[str, list[str]] = {}
        for _module, node in tool_catalog.iter_decorated_functions(TOOLS_DIR):
            documented = self._args_entries(ast.get_docstring(node) or "")
            signature = set(tool_catalog._params_of(node))
            ghosts = documented - signature
            if ghosts:
                problems[node.name] = sorted(ghosts)
        self.assertEqual(problems, {}, f"docstrings document non-existent parameters: {problems}")

    def test_instructions_name_only_files_that_exist(self):
        """Guards the archive_mcp_client.py class of bug: a path that was already deleted."""
        text = tool_catalog.render_instructions(tool_catalog.discover_tools(TOOLS_DIR))
        banner = (REPO_ROOT / "Python" / "unreal_mcp_server.py").read_text(encoding="utf-8")
        for referenced in re.findall(r"`?(Python/[A-Za-z0-9_./]+\.py)`?", text + banner):
            self.assertTrue((REPO_ROOT / referenced).exists(),
                            f"instructions name a file that does not exist: {referenced}")

    def test_hygiene_checks_detect_a_planted_ghost(self):
        """Guard the guard: a docstring naming a parameter that does not exist is caught."""
        ghost_doc = "Do a thing.\n\nArgs:\n    not_a_real_parameter: nope\n"
        self.assertEqual(self._args_entries(ghost_doc) - {"blueprint_name"}, {"not_a_real_parameter"})


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
