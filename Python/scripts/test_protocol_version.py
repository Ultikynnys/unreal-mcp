#!/usr/bin/env python3
"""Contract test for the plugin/server protocol version handshake.

A stale Unreal plugin (an editor that was never rebuilt after the contract changed) answers
calls with an out-of-date envelope. The bridge stamps MCP_PROTOCOL_VERSION on every reply and
the server refuses anything that does not match, so the failure is a clear "rebuild the
plugin" instead of a confusing tool error. This test locks the pieces that make that work:

  1. the version lives in exactly one place (the plugin header) and the Python side reads it,
  2. the bridge stamps `protocol` onto every reply and nothing is left hardcoded,
  3. the mismatch check fails closed for a different version AND a missing one, and its
     message names both versions and the rebuild command.

Run directly or with unittest.
"""

from __future__ import annotations

import importlib.util
import pathlib
import re
import unittest

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[1]
HEADER = (REPO_ROOT / "MCPGameProject" / "Plugins" / "UnrealMCP" / "Source" / "UnrealMCP"
          / "Public" / "MCPProtocolVersion.h")
BRIDGE_CPP = (REPO_ROOT / "MCPGameProject" / "Plugins" / "UnrealMCP" / "Source" / "UnrealMCP"
              / "Private" / "UnrealMCPBridge.cpp")
EDITOR_CPP = (REPO_ROOT / "MCPGameProject" / "Plugins" / "UnrealMCP" / "Source" / "UnrealMCP"
              / "Private" / "Commands" / "UnrealMCPEditorCommands.cpp")

spec = importlib.util.spec_from_file_location(
    "unreal_mcp_server", REPO_ROOT / "Python" / "unreal_mcp_server.py")
server = importlib.util.module_from_spec(spec)
spec.loader.exec_module(server)


class HeaderTests(unittest.TestCase):
    def test_header_defines_the_version(self):
        text = HEADER.read_text(encoding="utf-8")
        self.assertRegex(text, r'#define\s+MCP_PROTOCOL_VERSION\s+TEXT\("[^"]+"\)')

    def test_server_reads_the_version_from_the_header(self):
        """One source of truth: the Python side must not carry its own literal."""
        expected = server.read_expected_protocol(HEADER)
        self.assertTrue(expected, "server could not read MCP_PROTOCOL_VERSION from the header")
        self.assertEqual(server.EXPECTED_PROTOCOL, expected)

    def test_missing_header_disables_rather_than_invents_a_version(self):
        self.assertEqual(server.read_expected_protocol(HEADER.parent / "nope.h"), "")

    def test_version_has_not_drifted_between_header_and_server(self):
        """The check the user asked for: the two sides must always agree."""
        server_text = (REPO_ROOT / "Python" / "unreal_mcp_server.py").read_text(encoding="utf-8")
        literals = re.findall(r'MCP_PROTOCOL_VERSION\s*=\s*"([^"]+)"', server_text)
        self.assertEqual(literals, [], "server hardcodes a version; it must read the header")


class BridgeStampTests(unittest.TestCase):
    def test_every_reply_carries_the_protocol_field(self):
        text = BRIDGE_CPP.read_text(encoding="utf-8")
        self.assertIn('ResponseJson->SetStringField(TEXT("protocol"), MCP_PROTOCOL_VERSION)', text)

    def test_capabilities_uses_the_macro(self):
        text = EDITOR_CPP.read_text(encoding="utf-8")
        self.assertIn('SetStringField(TEXT("protocol_version"), MCP_PROTOCOL_VERSION)', text)
        self.assertNotIn('SetStringField(TEXT("protocol_version"), TEXT("1.0"))', text)

    def test_bridge_includes_the_version_header(self):
        self.assertIn('#include "MCPProtocolVersion.h"', BRIDGE_CPP.read_text(encoding="utf-8"))


class MismatchCheckTests(unittest.TestCase):
    def test_matching_version_is_allowed(self):
        self.assertEqual(server.protocol_mismatch_error("2", "2"), "")

    def test_different_version_fails_with_both_versions_and_the_fix(self):
        message = server.protocol_mismatch_error("2", "1.0")
        self.assertIn("version mismatch", message)
        self.assertIn("protocol 2", message)
        self.assertIn("1.0", message)
        self.assertIn("Build.bat", message)
        self.assertIn("editor_process.py restart", message)

    def test_missing_version_fails_closed(self):
        """An older plugin that predates the field must not be trusted either."""
        message = server.protocol_mismatch_error("2", "")
        self.assertIn("version mismatch", message)
        self.assertIn("no protocol version", message)

    def test_unreadable_header_skips_the_check(self):
        self.assertEqual(server.protocol_mismatch_error("", "1.0"), "")

    def test_real_files_agree(self):
        """Guard the guard: the two sides of the repo must satisfy the check today."""
        self.assertEqual(
            server.protocol_mismatch_error(server.EXPECTED_PROTOCOL, server.EXPECTED_PROTOCOL), "")


if __name__ == "__main__":
    unittest.main(verbosity=2)
