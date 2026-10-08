#!/usr/bin/env python3
"""Contract test for the agent-facing MCP tool surface.

The MCP tool layer IS the agent's API: its parameters become the JSON schema the model
sees, and its docstring is the description the model reads. This test locks three
properties that a careless edit would quietly break:

  1. every tool has a non-empty docstring (the agent has nothing else to go on),
  2. every parameter is typed with a concrete type - no Optional[...] / Union[...] /
     Any / object, and no untyped parameter - so the generated schema is a plain type,
     not an untyped blob. Three genuinely polymorphic value parameters are allowlisted
     because forcing them to a concrete type would make FastMCP reject numbers/bools,
  3. every tool sends a command name, and each name is a command the C++ plugin knows
     (cross-checked against check_tool_parity's bridge scan).

Exits non-zero on any violation. Run with `unittest` or directly.
"""

from __future__ import annotations

import ast
import importlib.util
import pathlib
import re
import sys
import tempfile
import textwrap
import unittest

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[1]
TOOLS_DIR = REPO_ROOT / "Python" / "tools"

FORBIDDEN_OUTER_TYPES = ("Optional", "Union", "Any", "object")

# Parameters that are genuinely polymorphic JSON scalars (a string, number or bool,
# forwarded verbatim). A concrete annotation would make FastMCP reject the other types,
# so they are allowed to stay untyped; every other parameter must be typed.
POLYMORPHIC_PARAMS: dict[str, set[str]] = {
    "set_actor_property": {"property_value"},
    "set_blueprint_property": {"property_value"},
    "set_component_property": {"property_value"},
    "set_blueprint_node_pin_default": {"value"},
}

SEND_COMMAND_RE = re.compile(r'send_command\(\s*["\']([a-z0-9_]+)["\']')


def _iter_tool_defs(module_path: pathlib.Path):
    """Yield (function_name, docstring, params, command) for each MCP tool in a module.

    A tool is a function whose body calls send_command(...); params is a list of
    (name, annotation_text_or_None).
    """
    tree = ast.parse(module_path.read_text(encoding="utf-8"))

    def walk(node):
        for child in ast.walk(node):
            if isinstance(child, ast.FunctionDef):
                calls = [n for n in ast.walk(child)
                         if isinstance(n, ast.Call)
                         and isinstance(n.func, ast.Attribute)
                         and n.func.attr == "send_command"]
                if not calls:
                    continue
                command = None
                if calls[0].args and isinstance(calls[0].args[0], ast.Constant):
                    command = calls[0].args[0].value
                args = list(child.args.posonlyargs) + list(child.args.args) + list(child.args.kwonlyargs)
                params = []
                for a in args:
                    if a.arg in ("ctx", "self"):
                        continue
                    annotation = ast.unparse(a.annotation) if a.annotation else None
                    params.append((a.arg, annotation))
                yield child.name, ast.get_docstring(child), params, command

    yield from walk(tree)


def analyze_tools(tools_dir: pathlib.Path) -> dict[str, list[str]]:
    """Return {tool_name: [violations]} for every tool found under tools_dir."""
    findings: dict[str, list[str]] = {}
    for module_path in sorted(tools_dir.glob("*.py")):
        for name, docstring, params, command in _iter_tool_defs(module_path):
            problems: list[str] = []
            if not docstring or not docstring.strip():
                problems.append("missing docstring")
            if not command:
                problems.append("no send_command name")
            allowlisted = POLYMORPHIC_PARAMS.get(name, set())
            for param_name, annotation in params:
                if param_name in allowlisted:
                    continue
                if annotation is None:
                    problems.append(f"param '{param_name}' is untyped")
                    continue
                # Only the OUTER type matters: Dict[str, Any] / List[Dict[str, Any]] are
                # legitimate JSON object/array schemas, whereas a top-level Optional/Union/
                # Any/object makes the schema an untyped or nullable blob.
                outer = annotation.split("[", 1)[0].strip()
                if outer in FORBIDDEN_OUTER_TYPES:
                    problems.append(f"param '{param_name}' uses forbidden outer type: {annotation}")
            if problems:
                findings[name] = problems
    return findings


def collect_tool_commands(tools_dir: pathlib.Path) -> dict[str, str]:
    """{tool_name: command} for every tool under tools_dir."""
    commands: dict[str, str] = {}
    for module_path in sorted(tools_dir.glob("*.py")):
        for name, _doc, _params, command in _iter_tool_defs(module_path):
            if command:
                commands[name] = command
    return commands


def _load_parity_module():
    spec = importlib.util.spec_from_file_location(
        "check_tool_parity", SCRIPT_DIR / "check_tool_parity.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ToolSurfaceTests(unittest.TestCase):
    def test_tools_are_discovered(self):
        commands = collect_tool_commands(TOOLS_DIR)
        self.assertGreater(len(commands), 50, "tool discovery found too few tools")

    def test_no_surface_violations(self):
        findings = analyze_tools(TOOLS_DIR)
        self.assertEqual(findings, {}, f"agent-facing surface violations: {findings}")

    def test_every_tool_command_is_known_to_the_plugin(self):
        parity = _load_parity_module()
        known, _ = parity.collect_cpp_commands(REPO_ROOT)
        commands = collect_tool_commands(TOOLS_DIR)
        unknown = {tool: cmd for tool, cmd in commands.items() if cmd not in known}
        self.assertEqual(unknown, {}, f"tools sending unknown commands: {unknown}")

    def test_violations_are_actually_detected(self):
        """The analyzer must fail on a synthetically broken tool (guards the guard)."""
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            (tmp / "bad_tools.py").write_text(textwrap.dedent('''
                from typing import Optional, Any
                def register_bad_tools(mcp):
                    @mcp.tool()
                    def undocumented(ctx, thing, value: Any) -> dict:
                        return ctx.send_command("undocumented", {"thing": thing})
            '''), encoding="utf-8")
            findings = analyze_tools(tmp)
            self.assertIn("undocumented", findings)
            problems = findings["undocumented"]
            self.assertTrue(any("docstring" in p for p in problems))
            self.assertTrue(any("forbidden" in p for p in problems))


if __name__ == "__main__":
    unittest.main(verbosity=2)
