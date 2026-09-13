#!/usr/bin/env python3
"""Tests for check_tool_parity.py.

These confirm the parity claim is actually *tested*: the checker must PASS when the
Python tool layer and the C++ plugin are in sync, and FAIL when a Python command has
no C++ handler. Run in CI before the parity check itself.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve()
REPO_ROOT = SCRIPT.parents[2]

BRIDGE_REL = "MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/UnrealMCPBridge.cpp"
PY_TOOL_REL = "Python/tools/editor_tools.py"


def _write(root: pathlib.Path, rel: str, text: str) -> None:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def _make_bridge(root: pathlib.Path, commands: list[str]) -> None:
    body = "\n".join(f'    if (CommandType == TEXT("{c}")) {{ /* handler */ }}' for c in commands)
    _write(root, BRIDGE_REL,
           "FString UUnrealMCPBridge::ExecuteCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)\n"
           "{\n" + body + "\n}\n")


def _make_py_tool(root: pathlib.Path, commands: list[str]) -> None:
    body = "\n".join(f'    resp = unreal.send_command("{c}", {{}})' for c in commands)
    _write(root, PY_TOOL_REL,
           "def register_editor_tools(mcp):\n" + (body or "    pass") + "\n")


class ParityCheckTests(unittest.TestCase):
    def _run(self, root: pathlib.Path) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(SCRIPT.parent / "check_tool_parity.py"),
             "--root", str(root), "--quiet"],
            capture_output=True, text=True,
        )

    def test_in_sync_passes(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            _make_bridge(tmp, ["get_actors_in_level"])
            _make_py_tool(tmp, ["get_actors_in_level"])
            result = self._run(tmp)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_missing_cpp_handler_fails(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            _make_bridge(tmp, ["get_actors_in_level"])
            _make_py_tool(tmp, ["get_actors_in_level", "spawn_unicorn"])
            result = self._run(tmp)
            self.assertEqual(result.returncode, 1,
                             "checker must fail when a Python command has no C++ handler")
            self.assertIn("spawn_unicorn", result.stdout)

    def test_extra_cpp_command_is_allowed(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            _make_bridge(tmp, ["get_actors_in_level", "ping"])
            _make_py_tool(tmp, ["get_actors_in_level"])
            result = self._run(tmp)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_real_repo_has_parity(self):
        """The actual fork must satisfy the parity contract."""
        result = self._run(REPO_ROOT)
        self.assertEqual(result.returncode, 0,
                         "fork parity broken:\n" + result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
