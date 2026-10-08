#!/usr/bin/env python3
"""Unit tests for editor_process.py's pure helpers.

The reaper must never kill an editor for a different project, so the filter that
decides "ours" is the part worth pinning. These tests run with no editor and no
PowerShell output, only fixtures.
"""

from __future__ import annotations

import importlib.util
import json
import pathlib
import tempfile
import unittest

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("editor_process", SCRIPT_DIR / "editor_process.py")
editor_process = importlib.util.module_from_spec(spec)
spec.loader.exec_module(editor_process)

UPROJECT = pathlib.Path(r"C:\proj\MCPGameProject\MCPGameProject.uproject")
OURS = r'"C:\Program Files\Epic Games\UE_5.6\Engine\Binaries\Win64\UnrealEditor.exe" C:\proj\MCPGameProject\MCPGameProject.uproject -log'
FOREIGN = r'"C:\Program Files\Epic Games\UE_5.6\Engine\Binaries\Win64\UnrealEditor.exe" D:\other\Other.uproject -log'


class ParseProcessesTests(unittest.TestCase):
    def test_empty_output(self):
        self.assertEqual(editor_process.parse_processes(""), [])
        self.assertEqual(editor_process.parse_processes("   \n"), [])

    def test_single_object_json(self):
        raw = json.dumps({"ProcessId": 10, "CreationDate": "/Date(1)/", "CommandLine": OURS})
        self.assertEqual(editor_process.parse_processes(raw)[0]["pid"], 10)

    def test_array_json(self):
        raw = json.dumps([{"ProcessId": 10, "CommandLine": OURS}, {"ProcessId": 11, "CommandLine": FOREIGN}])
        self.assertEqual([p["pid"] for p in editor_process.parse_processes(raw)], [10, 11])

    def test_invalid_json_and_missing_pid(self):
        self.assertEqual(editor_process.parse_processes("not json"), [])
        self.assertEqual(editor_process.parse_processes(json.dumps({"CommandLine": OURS})), [])


class SelectProjectEditorsTests(unittest.TestCase):
    def test_selects_only_ours(self):
        procs = [{"pid": 10, "command_line": OURS}, {"pid": 11, "command_line": FOREIGN}]
        self.assertEqual(editor_process.select_project_editors(procs, UPROJECT), [10])

    def test_matching_is_case_and_separator_insensitive(self):
        procs = [{"pid": 10, "command_line": r'"...\unrealeditor.exe" C:/PROJ/mcpgameproject/MCPGameProject.uproject'}]
        self.assertEqual(editor_process.select_project_editors(procs, UPROJECT), [10])

    def test_no_editors_selected_when_none_match(self):
        procs = [{"pid": 11, "command_line": FOREIGN}]
        self.assertEqual(editor_process.select_project_editors(procs, UPROJECT), [])


class CrashReportTests(unittest.TestCase):
    def test_newest_wins(self):
        reports = [("old", 1.0), ("new", 3.0), ("mid", 2.0)]
        self.assertEqual(editor_process.newest_crash(reports)[0], "new")

    def test_empty(self):
        self.assertIsNone(editor_process.newest_crash([]))


class CimDateTests(unittest.TestCase):
    def test_converts_ms_since_epoch(self):
        rendered = editor_process.format_cim_date("/Date(1791426948467)/")
        self.assertRegex(rendered, r"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$")

    def test_passthrough_for_unknown(self):
        self.assertEqual(editor_process.format_cim_date(""), "unknown")
        self.assertEqual(editor_process.format_cim_date("garbage"), "garbage")


class SecretTests(unittest.TestCase):
    def test_reads_secret_from_server_source(self):
        with tempfile.TemporaryDirectory() as d:
            server = pathlib.Path(d) / "unreal_mcp_server.py"
            server.write_text('CONTROL_PLANE_SECRET = "deadbeef00"\n', encoding="utf-8")
            self.assertEqual(editor_process.control_plane_secret(server), "deadbeef00")

    def test_missing_secret_is_empty(self):
        with tempfile.TemporaryDirectory() as d:
            server = pathlib.Path(d) / "unreal_mcp_server.py"
            server.write_text("# no secret here\n", encoding="utf-8")
            self.assertEqual(editor_process.control_plane_secret(server), "")


if __name__ == "__main__":
    unittest.main(verbosity=2)
