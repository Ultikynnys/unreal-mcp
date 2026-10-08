#!/usr/bin/env python3
"""Unit tests for editor_process.py's pure helpers.

The reaper must never kill an editor for a different project, so the filter that
decides "ours" is the part worth pinning. These tests run with no editor and no
PowerShell output, only fixtures.
"""

from __future__ import annotations

import importlib.util
import json
import os
import pathlib
import tempfile
import time
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


class ClassifyProcessTests(unittest.TestCase):
    @staticmethod
    def _proc(pid, name, cmd):
        return {"pid": pid, "name": name, "created": "", "command_line": cmd}

    def test_editor_ours_vs_foreign(self):
        procs = [self._proc(10, "UnrealEditor.exe", OURS), self._proc(11, "UnrealEditor.exe", FOREIGN)]
        tagged = editor_process.classify_processes(procs, UPROJECT, {10, 11})
        self.assertEqual([(p["pid"], p["kind"], p["ours"]) for p in tagged],
                         [(10, "editor", True), (11, "editor", False)])

    def test_editor_match_is_case_and_separator_insensitive(self):
        procs = [self._proc(10, "UnrealEditor.exe",
                            r'"x\UnrealEditor.exe" C:/PROJ/mcpgameproject/MCPGameProject.uproject')]
        self.assertTrue(editor_process.classify_processes(procs, UPROJECT, {10})[0]["ours"])

    def test_helper_with_live_owner_is_not_stale(self):
        procs = [self._proc(20, "CrashReportClientEditor.exe", "x -MONITOR=10 -unattended")]
        tagged = editor_process.classify_processes(procs, UPROJECT, {10, 20})[0]
        self.assertEqual(tagged["owner"], 10)
        self.assertFalse(tagged["stale"])

    def test_helper_with_dead_owner_is_orphan(self):
        procs = [self._proc(21, "CrashReportClientEditor.exe", "x -MONITOR=999 -RespawnedInstance")]
        self.assertTrue(editor_process.classify_processes(procs, UPROJECT, {21})[0]["stale"])

    def test_helper_without_owner_is_orphan(self):
        procs = [self._proc(22, "UnrealTraceServer.exe", "daemon -d")]
        tagged = editor_process.classify_processes(procs, UPROJECT, {22})[0]
        self.assertIsNone(tagged["owner"])
        self.assertTrue(tagged["stale"])


class SnapshotTests(unittest.TestCase):
    def test_parse_valid(self):
        snap = editor_process.parse_snapshot('{"state": "busy", "command": "save_level"}')
        self.assertEqual(snap["state"], "busy")

    def test_parse_missing_or_malformed(self):
        self.assertIsNone(editor_process.parse_snapshot(""))
        self.assertIsNone(editor_process.parse_snapshot("not json"))
        self.assertIsNone(editor_process.parse_snapshot("[1, 2]"))

    def test_describe_busy_names_command_stall_and_modal(self):
        text = editor_process.describe_snapshot({
            "state": "busy", "command": "save_level", "command_elapsed_seconds": 41.0,
            "game_thread_stalled_seconds": 41.2, "modal_title": "Redirector Update Report",
        })
        self.assertIn("state=busy", text)
        self.assertIn("save_level", text)
        self.assertIn("game_thread_stalled=41.2s", text)
        self.assertIn("modal='Redirector Update Report'", text)

    def test_describe_idle_and_absent(self):
        idle = editor_process.describe_snapshot({"state": "idle", "last_command": "ping",
                                                 "last_command_seconds": 0.1,
                                                 "game_thread_stalled_seconds": 0.2})
        self.assertIn("last='ping'", idle)
        self.assertIn("no snapshot", editor_process.describe_snapshot(None))


class OwnerPidTests(unittest.TestCase):
    def test_monitor_flag(self):
        self.assertEqual(editor_process.parse_owner_pid("x -MONITOR=33752 -y"), 33752)

    def test_sponsor_flag_variants(self):
        self.assertEqual(editor_process.parse_owner_pid("daemon -d --sponsor 28956"), 28956)
        self.assertEqual(editor_process.parse_owner_pid("daemon --sponsor=28956"), 28956)

    def test_none_when_absent(self):
        self.assertIsNone(editor_process.parse_owner_pid("no owner here"))


class SnapshotFreshnessTests(unittest.TestCase):
    """A leftover snapshot must not be reported as the current state."""

    @staticmethod
    def _write(directory: str) -> pathlib.Path:
        path = pathlib.Path(directory) / "bridge_state.json"
        path.write_text("{}", encoding="utf-8")
        return path

    def test_recent_file_is_fresh(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertTrue(editor_process.snapshot_is_fresh(self._write(d), time.time()))

    def test_old_file_is_stale(self):
        with tempfile.TemporaryDirectory() as d:
            path = self._write(d)
            old = time.time() - 600
            os.utime(path, (old, old))
            self.assertFalse(editor_process.snapshot_is_fresh(path, time.time()))

    def test_missing_file_is_stale(self):
        self.assertFalse(editor_process.snapshot_is_fresh(
            pathlib.Path("does_not_exist.json"), time.time()))


class ReapTargetTests(unittest.TestCase):
    def test_default_targets_our_editors_and_stale_helpers_only(self):
        tagged = [
            {"pid": 10, "kind": "editor", "ours": True, "stale": False},
            {"pid": 11, "kind": "editor", "ours": False, "stale": False},
            {"pid": 20, "kind": "helper", "ours": False, "stale": True},
            {"pid": 21, "kind": "helper", "ours": False, "stale": False},
        ]
        self.assertEqual(editor_process.select_reap_targets(tagged), [10, 20])

    def test_all_targets_everything(self):
        tagged = [
            {"pid": 10, "kind": "editor", "ours": True, "stale": False},
            {"pid": 11, "kind": "editor", "ours": False, "stale": False},
            {"pid": 21, "kind": "helper", "ours": False, "stale": False},
        ]
        self.assertEqual(editor_process.select_reap_targets(tagged, reap_all=True), [10, 11, 21])

    def test_orphans_only_selects_stale_helpers(self):
        tagged = [
            {"pid": 10, "kind": "editor", "ours": True, "stale": False},
            {"pid": 20, "kind": "helper", "ours": False, "stale": True},
            {"pid": 21, "kind": "helper", "ours": False, "stale": False},
        ]
        self.assertEqual(editor_process.select_orphan_helpers(tagged), [20])


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


class OrphanWatchdogTests(unittest.TestCase):
    """The stdio server must not exit on its own; only a proven-dead launcher ends it."""

    @classmethod
    def setUpClass(cls):
        import importlib.util
        root = pathlib.Path(__file__).resolve().parents[2]
        spec = importlib.util.spec_from_file_location("server_watchdog", root / "Python" / "unreal_mcp_server.py")
        cls.server = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.server)
        cls.source = (root / "Python" / "unreal_mcp_server.py").read_text(encoding="utf-8")

    def test_a_live_process_reads_as_alive(self):
        self.assertTrue(self.server._process_alive(os.getppid()))

    def test_our_own_pid_reads_as_alive(self):
        self.assertTrue(self.server._process_alive(os.getpid()))

    def test_a_pid_that_cannot_exist_reads_as_dead(self):
        self.assertFalse(self.server._process_alive(999999999))

    def test_reparenting_does_not_end_the_server(self):
        """A wrapper launcher exiting reparents us; that must not be read as death."""
        self.assertNotIn("os.getppid() != parent_pid", self.source)

    def test_access_denied_is_not_death(self):
        self.assertIn("!= 87", self.source)

if __name__ == "__main__":
    unittest.main(verbosity=2)
