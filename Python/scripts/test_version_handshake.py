#!/usr/bin/env python3
"""Contract test for the plugin/server version handshake.

The plugin's identity is its OWN version, not the host project's commit. The plugin reports the
VersionName from its `.uplugin` descriptor and the server declares the version it speaks, so a
plugin build is portable across projects. This test locks the pieces that make that work:

  1. the version is a plugin property (the `.uplugin`), never derived from git at build time,
  2. `.uplugin` VersionName and the server's PLUGIN_VERSION agree - the battery cannot drift,
  3. the bridge stamps `plugin_version` on every reply, read from the descriptor,
  4. the check fails closed for a different version, a missing one and "unknown", and it
     refuses BEFORE the command reaches the editor.

Run directly or with unittest.
"""

from __future__ import annotations

import importlib.util
import pathlib
import re
import unittest

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[1]
PLUGIN = REPO_ROOT / "MCPGameProject" / "Plugins" / "UnrealMCP"
UPLUGIN = PLUGIN / "UnrealMCP.uplugin"
BUILD_CS = PLUGIN / "Source" / "UnrealMCP" / "UnrealMCP.Build.cs"
OLD_HEADER = PLUGIN / "Source" / "UnrealMCP" / "Public" / "MCPBuildRevision.h"
VERSION_HEADER = PLUGIN / "Source" / "UnrealMCP" / "Public" / "MCPPluginVersion.h"
BRIDGE_CPP = PLUGIN / "Source" / "UnrealMCP" / "Private" / "UnrealMCPBridge.cpp"
EDITOR_CPP = PLUGIN / "Source" / "UnrealMCP" / "Private" / "Commands" / "UnrealMCPEditorCommands.cpp"

spec = importlib.util.spec_from_file_location(
    "unreal_mcp_server", REPO_ROOT / "Python" / "unreal_mcp_server.py")
server = importlib.util.module_from_spec(spec)
spec.loader.exec_module(server)


def uplugin_version() -> str:
    match = re.search(r'"VersionName"\s*:\s*"([^"]*)"', UPLUGIN.read_text(encoding="utf-8"))
    return match.group(1) if match else ""


class VersionSourceTests(unittest.TestCase):
    def test_the_manifest_and_the_server_agree(self):
        """The whole point of the battery: bumping one side without the other fails here."""
        self.assertEqual(uplugin_version(), server.PLUGIN_VERSION)

    def test_the_build_no_longer_shells_out_to_git(self):
        text = BUILD_CS.read_text(encoding="utf-8")
        for needle in ("rev-parse", "MCP_REVISION", "System.Diagnostics", "ProcessStartInfo",
                       "RunGit", "BuiltDirty"):
            self.assertNotIn(needle, text, f"UnrealMCP.Build.cs still references {needle}")

    def test_the_revision_header_is_gone(self):
        self.assertFalse(OLD_HEADER.exists(), "MCPBuildRevision.h should be gone")

    def test_the_server_derives_no_revision(self):
        for name in ("repo_revision", "_revision_from_git_dir", "revision_mismatch_error",
                     "REPO_ROOT", "_warn_built_dirty_once", "_dirty_warned"):
            self.assertFalse(hasattr(server, name), f"{name} should be gone")


class VersionSourceIsTheDescriptorTests(unittest.TestCase):
    def test_the_version_comes_from_the_plugin_descriptor(self):
        header = VERSION_HEADER.read_text(encoding="utf-8")
        self.assertIn('FindPlugin(TEXT("UnrealMCP"))', header)
        self.assertIn("GetDescriptor().VersionName", header)


class BridgeStampTests(unittest.TestCase):
    def test_every_reply_carries_the_plugin_version(self):
        self.assertIn(
            'ResponseJson->SetStringField(TEXT("plugin_version"), UnrealMCPPluginVersion())',
            BRIDGE_CPP.read_text(encoding="utf-8"))

    def test_capabilities_reports_the_plugin_version(self):
        self.assertIn(
            'ResultObj->SetStringField(TEXT("plugin_version"), UnrealMCPPluginVersion())',
            EDITOR_CPP.read_text(encoding="utf-8"))

    def test_no_revision_is_stamped_any_more(self):
        for path in (BRIDGE_CPP, EDITOR_CPP):
            text = path.read_text(encoding="utf-8")
            self.assertNotIn("MCP_REVISION", text)
            self.assertNotIn("built_dirty", text)


class MismatchMessageTests(unittest.TestCase):
    def test_matching_version_is_allowed(self):
        self.assertEqual(server.version_mismatch_error(server.PLUGIN_VERSION), "")

    def test_a_different_version_names_both_and_the_fix(self):
        message = server.version_mismatch_error("0.9.0")
        self.assertIn("version mismatch", message)
        self.assertIn(server.PLUGIN_VERSION, message)
        self.assertIn("0.9.0", message)
        self.assertIn("build_plugin.py", message)

    def test_a_missing_version_fails_closed(self):
        self.assertIn("predates", server.version_mismatch_error(""))

    def test_an_unreadable_version_fails_closed(self):
        self.assertIn("descriptor", server.version_mismatch_error("unknown"))


class PreflightOrderTests(unittest.TestCase):
    """The check must run BEFORE the command is dispatched.

    Validating the real command's own reply would be too late: the editor executes a command
    before it replies, so a plugin of a different version could still create or delete assets
    while the server only refused to report the result. This is a regression guard for that bug.
    """

    def _connection(self, replies, raise_on=()):
        conn = server.UnrealConnection()
        sent: list[str] = []

        def fake_dispatch(command, params=None, read_timeout=None):
            sent.append(command)
            if command in raise_on:
                raise RuntimeError("editor unreachable")
            return replies[command]

        conn._dispatch = fake_dispatch
        return conn, sent

    def test_a_different_plugin_version_refuses_before_dispatching(self):
        conn, sent = self._connection({"ping": {"plugin_version": "0.0.1"}})
        result = conn.send_command("create_blueprint", {"name": "X", "parent_class": "Actor"})
        self.assertFalse(result["success"])
        self.assertIn("version mismatch", result["message"])
        self.assertEqual(sent, ["ping"], "the mutating command must not reach the editor")

    def test_a_plugin_without_a_version_refuses_before_dispatching(self):
        conn, sent = self._connection({"ping": {"message": "pong"}})
        result = conn.send_command("delete_assets", {"asset_paths": ["/Game/X"]})
        self.assertFalse(result["success"])
        self.assertEqual(sent, ["ping"])

    def test_matching_version_dispatches_after_the_probe(self):
        conn, sent = self._connection({
            "ping": {"plugin_version": server.PLUGIN_VERSION},
            "get_capabilities": {"plugin_version": server.PLUGIN_VERSION,
                                 "status": "success", "result": {"ok": True}},
        })
        result = conn.send_command("get_capabilities")
        self.assertTrue(result["success"])
        self.assertEqual(sent, ["ping", "get_capabilities"])

    def test_ping_is_not_probed_again(self):
        conn, sent = self._connection({"ping": {"plugin_version": server.PLUGIN_VERSION}})
        conn.send_command("ping")
        self.assertEqual(sent, ["ping"])

    def test_transport_failure_is_not_reported_as_a_version_mismatch(self):
        conn, _sent = self._connection({"get_capabilities": {}}, raise_on=("ping",))
        result = conn.send_command("get_capabilities")
        self.assertFalse(result["success"])
        self.assertIn("unreachable", result["message"])
        self.assertNotIn("mismatch", result["message"])


class BoundedProbeTests(unittest.TestCase):
    """A probe that cannot be answered must fail fast, not hang past the host's own limit."""

    def test_the_probe_is_bounded_well_below_a_host_limit(self):
        self.assertGreaterEqual(server.PROBE_READ_TIMEOUT, 1)
        self.assertLess(server.PROBE_READ_TIMEOUT, 30)

    def test_an_editor_that_never_answers_fails_fast_and_says_where_to_look(self):
        import socket
        import threading
        import time

        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        port = listener.getsockname()[1]
        held = []

        def accept_and_never_reply():
            connection, _ = listener.accept()   # accepted, then silence
            held.append(connection)
            time.sleep(5)

        threading.Thread(target=accept_and_never_reply, daemon=True).start()

        original_port = server.UNREAL_PORT
        original_probe = server.PROBE_READ_TIMEOUT
        server.UNREAL_PORT = port
        server.PROBE_READ_TIMEOUT = 0.5
        try:
            started = time.time()
            result = server.UnrealConnection().send_command("get_capabilities")
            elapsed = time.time() - started
        finally:
            server.UNREAL_PORT = original_port
            server.PROBE_READ_TIMEOUT = original_probe
            for connection in held:
                connection.close()
            listener.close()

        self.assertFalse(result["success"])
        self.assertLess(elapsed, 10, "the probe must not wait out the long read timeout")
        self.assertIn("did not answer a ping", result["message"])
        self.assertIn("bridge_state.json", result["message"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
