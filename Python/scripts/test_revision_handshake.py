#!/usr/bin/env python3
"""Contract test for the plugin/server revision handshake.

A stale Unreal plugin (an editor never rebuilt after the plugin changed) answers calls with an
out-of-date contract. The plugin bakes in the commit it was BUILT from (UnrealMCP.Build.cs ->
MCP_REVISION) and the server compares it against the commit it RUNS from, refusing before
dispatch, so the failure is a clear "rebuild the plugin" instead of a confusing tool error.
This test locks the pieces that make that work:

  1. the revision reaches the plugin from the build file, not a hand-maintained constant,
  2. the bridge stamps `revision` (and `built_dirty`) onto every reply,
  3. the server derives its own revision from git,
  4. the check fails closed for a different revision, a missing one and "unknown", and it
     refuses BEFORE the command reaches the editor.

Run directly or with unittest.
"""

from __future__ import annotations

import importlib.util
import os
import pathlib
import subprocess
import unittest
from unittest import mock

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[1]
PLUGIN = REPO_ROOT / "MCPGameProject" / "Plugins" / "UnrealMCP" / "Source" / "UnrealMCP"
BUILD_CS = PLUGIN / "UnrealMCP.Build.cs"
HEADER = PLUGIN / "Public" / "MCPBuildRevision.h"
OLD_HEADER = PLUGIN / "Public" / "MCPProtocolVersion.h"
BRIDGE_CPP = PLUGIN / "Private" / "UnrealMCPBridge.cpp"
EDITOR_CPP = PLUGIN / "Private" / "Commands" / "UnrealMCPEditorCommands.cpp"

spec = importlib.util.spec_from_file_location(
    "unreal_mcp_server", REPO_ROOT / "Python" / "unreal_mcp_server.py")
server = importlib.util.module_from_spec(spec)
spec.loader.exec_module(server)


def head_revision() -> str:
    return subprocess.run(
        ["git", "-C", str(REPO_ROOT), "rev-parse", "HEAD"],
        capture_output=True, text=True, check=True).stdout.strip()


class BuildSystemTests(unittest.TestCase):
    def test_build_injects_the_commit(self):
        text = BUILD_CS.read_text(encoding="utf-8")
        self.assertIn("rev-parse HEAD", text)
        self.assertIn('Definitions.Add("MCP_REVISION=', text)
        self.assertIn("MCP_REVISION_DIRTY", text)

    def test_build_honours_the_env_override(self):
        """A plugin copied into another project pins the revision of the repo it came from."""
        self.assertIn('GetEnvironmentVariable("MCP_REVISION")', BUILD_CS.read_text(encoding="utf-8"))

    def test_header_is_only_a_fallback(self):
        text = HEADER.read_text(encoding="utf-8")
        self.assertIn("#ifndef MCP_REVISION", text)
        self.assertIn('TEXT("unknown")', text)

    def test_the_hand_incremented_header_is_gone(self):
        self.assertFalse(OLD_HEADER.exists(), "MCPProtocolVersion.h should be gone")
        for path in (BRIDGE_CPP, EDITOR_CPP, HEADER):
            self.assertNotIn("MCP_PROTOCOL_VERSION", path.read_text(encoding="utf-8"))


class BridgeStampTests(unittest.TestCase):
    def test_every_reply_carries_the_revision(self):
        text = BRIDGE_CPP.read_text(encoding="utf-8")
        self.assertIn('ResponseJson->SetStringField(TEXT("revision"), MCP_REVISION)', text)
        self.assertIn('ResponseJson->SetBoolField(TEXT("built_dirty"), MCP_REVISION_DIRTY != 0)', text)

    def test_capabilities_reports_the_revision(self):
        text = EDITOR_CPP.read_text(encoding="utf-8")
        self.assertIn('SetStringField(TEXT("revision"), MCP_REVISION)', text)

    def test_bridge_includes_the_revision_header(self):
        self.assertIn('#include "MCPBuildRevision.h"', BRIDGE_CPP.read_text(encoding="utf-8"))


class ServerRevisionTests(unittest.TestCase):
    def test_server_revision_is_this_checkout(self):
        self.assertRegex(server.REPO_REVISION, r"^[0-9a-f]{40}$")
        self.assertEqual(server.REPO_REVISION, head_revision())

    def test_env_override_wins(self):
        with mock.patch.dict(os.environ, {"UNREAL_MCP_REVISION": "cafebabe"}):
            self.assertEqual(server.repo_revision(), "cafebabe")

    def test_the_removed_constants_are_gone(self):
        for name in ("SERVER_PROTOCOL", "HEADER_PROTOCOL", "read_expected_protocol",
                     "protocol_agreement_error", "protocol_mismatch_error"):
            self.assertFalse(hasattr(server, name), f"{name} should be gone")


class MismatchMessageTests(unittest.TestCase):
    def test_matching_revision_is_allowed(self):
        self.assertEqual(server.revision_mismatch_error(head_revision()), "")

    def test_different_revision_names_both_sides_and_the_fix(self):
        message = server.revision_mismatch_error("deadbeef")
        self.assertIn("revision mismatch", message)
        self.assertIn(head_revision(), message)
        self.assertIn("deadbeef", message)
        self.assertIn("Build.bat", message)
        self.assertIn("editor_process.py restart", message)

    def test_missing_revision_fails_closed(self):
        self.assertIn("no revision at all", server.revision_mismatch_error(""))

    def test_unknown_revision_fails_closed(self):
        """Built where git was unavailable: the revision identifies nothing."""
        self.assertIn("built without a revision", server.revision_mismatch_error("unknown"))


class PreflightOrderTests(unittest.TestCase):
    """The check must run BEFORE the command is dispatched.

    Validating the real command's own reply would be too late: the editor executes a command
    before it replies, so a stale plugin could still create or delete assets while the server
    only refused to report the result. This is a regression guard for exactly that bug.
    """

    def _connection(self, replies, raise_on=()):
        conn = server.UnrealConnection()
        sent: list[str] = []

        def fake_dispatch(command, params=None):
            sent.append(command)
            if command in raise_on:
                raise RuntimeError("editor unreachable")
            return replies[command]

        conn._dispatch = fake_dispatch
        return conn, sent

    def test_stale_plugin_refuses_before_dispatching_a_mutating_command(self):
        conn, sent = self._connection({"ping": {"revision": "0000000000000000000000000000000000000000"}})
        result = conn.send_command("create_blueprint", {"name": "X", "parent_class": "Actor"})
        self.assertFalse(result["success"])
        self.assertIn("revision mismatch", result["message"])
        self.assertEqual(sent, ["ping"], "the mutating command must not reach the editor")

    def test_plugin_without_a_revision_refuses_before_dispatching(self):
        conn, sent = self._connection({"ping": {"message": "pong"}})
        result = conn.send_command("delete_assets", {"asset_paths": ["/Game/X"]})
        self.assertFalse(result["success"])
        self.assertEqual(sent, ["ping"])

    def test_matching_revision_dispatches_after_the_probe(self):
        conn, sent = self._connection({
            "ping": {"revision": server.REPO_REVISION},
            "get_capabilities": {"revision": server.REPO_REVISION,
                                 "status": "success", "result": {"ok": True}},
        })
        result = conn.send_command("get_capabilities")
        self.assertTrue(result["success"])
        self.assertEqual(sent, ["ping", "get_capabilities"])

    def test_ping_is_not_probed_again(self):
        conn, sent = self._connection({"ping": {"revision": server.REPO_REVISION}})
        conn.send_command("ping")
        self.assertEqual(sent, ["ping"])

    def test_transport_failure_is_not_reported_as_a_revision_mismatch(self):
        conn, _sent = self._connection({"get_capabilities": {}}, raise_on=("ping",))
        result = conn.send_command("get_capabilities")
        self.assertFalse(result["success"])
        self.assertIn("unreachable", result["message"])
        self.assertNotIn("mismatch", result["message"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
