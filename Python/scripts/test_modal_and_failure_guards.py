#!/usr/bin/env python3
"""Regression lock for the bridge's modal/failure guards.

These guards exist because Unreal opens blocking windows that stall the game thread,
and because a failure that reaches the caller without a reason is useless to an agent.
They are cheap to delete by accident, so this test pins them by shape:

  1. dispatch runs under `TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true)`
     (an engine prompt auto-answers its default instead of opening a modal);
  2. a modal already on screen produces `EDITOR_MODAL_ACTIVE` rather than a hang, and
     recover_editor is exempt (it is the dismissal path);
  3. a failed command still carries the handler payload (`result`);
  4. batch_execute derives `success` from its failure count.

Fails when any of these is removed or renamed. Run directly or via unittest.
"""

from __future__ import annotations

import pathlib
import textwrap
import unittest

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[1]

BRIDGE = "MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/UnrealMCPBridge.cpp"
EDITOR_CMDS = "MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/Commands/UnrealMCPEditorCommands.cpp"

# Each check: (label, required literal). The literal is matched exactly so a rename
# or deletion fails the test rather than silently keeping a weaker guard.
BRIDGE_CHECKS = [
    ("unattended guard", "TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true)"),
    ("modal lookup", "FSlateApplication::Get().GetActiveModalWindow()"),
    ("modal error code", 'TEXT("EDITOR_MODAL_ACTIVE")'),
    ("modal recovery hint", "recover_editor to dismiss it"),
    ("recover_editor exempt from modal pre-flight", 'CommandType == TEXT("recover_editor")'),
    ("failure keeps handler payload", 'ResponseJson->SetObjectField(TEXT("result"), ResultJson);'),
    ("failure reason fallback", 'TEXT("Command failed without a reason; see \'result\'")'),
]

EDITOR_CMD_CHECKS = [
    ("batch success from failures", 'ResultObj->SetBoolField(TEXT("success"), Failures == 0);'),
    ("batch names failed actions", "batch action(s) failed:"),
]


def _missing(root: pathlib.Path) -> list[str]:
    """Return the labels of every guard that is absent from the sources under root."""
    bridge = (root / BRIDGE)
    editor = (root / EDITOR_CMDS)
    bridge_text = bridge.read_text(encoding="utf-8", errors="replace") if bridge.exists() else ""
    editor_text = editor.read_text(encoding="utf-8", errors="replace") if editor.exists() else ""

    missing = [label for label, literal in BRIDGE_CHECKS if literal not in bridge_text]
    missing += [label for label, literal in EDITOR_CMD_CHECKS if literal not in editor_text]
    return missing


class ModalAndFailureGuardTests(unittest.TestCase):
    def test_real_repo_has_all_guards(self):
        missing = _missing(REPO_ROOT)
        self.assertEqual(missing, [], f"missing guards: {missing}")

    def test_detects_a_removed_guard(self):
        """Guard-the-guard: a tree with the guard stripped must fail."""
        import tempfile

        with tempfile.TemporaryDirectory() as d:
            root = pathlib.Path(d)
            (root / BRIDGE).parent.mkdir(parents=True, exist_ok=True)
            (root / EDITOR_CMDS).parent.mkdir(parents=True, exist_ok=True)
            # A bridge with the modal pre-flight removed entirely.
            (root / BRIDGE).write_text(textwrap.dedent('''
                TSharedPtr<FJsonObject> UUnrealMCPBridge::ExecuteCommand(...)
                {
                    TSharedPtr<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
                    return ResponseJson;
                }
            '''), encoding="utf-8")
            (root / EDITOR_CMDS).write_text("// no guards here\n", encoding="utf-8")

            missing = _missing(root)
            self.assertIn("unattended guard", missing)
            self.assertIn("modal lookup", missing)
            self.assertIn("batch success from failures", missing)


if __name__ == "__main__":
    unittest.main(verbosity=2)
