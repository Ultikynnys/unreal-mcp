#!/usr/bin/env python3
"""Regression lock for the bridge's modal/failure guards.

These guards exist because Unreal opens blocking windows that stall the game thread,
and because a failure that reaches the caller without a reason is useless to an agent.
They are cheap to delete by accident, so this test pins them by shape:

  1. dispatch runs under `TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true)`
     (an engine prompt auto-answers its default instead of opening a modal) and
     `TGuardValue<bool> SilentGuard(GIsSilent, true)` (no slow-task progress window is raised);
  2. the refusal is keyed on a STALE GAME-THREAD HEARTBEAT, not on the mere presence of a window:
     a progress window or a passive prompt leaves the thread ticking and is dispatched through, and
     only a genuinely stuck thread is refused (`EDITOR_MODAL_ACTIVE` when a window is up,
     `EDITOR_BLOCKED` otherwise), with the window's class named and recover_editor exempt;
  3. a failed command still carries the handler payload (`result`);
  4. batch_execute derives `success` from its failure count;
  5. a finished move batch resaves packages the registry still reports as importing a moved path
     (a referencer moved together with its dependency is saved while the dependency is still at the
     old path, and only a re-save fixes that on disk).

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
SNAPSHOT = "MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/MCPStateSnapshot.cpp"
SNAPSHOT_H = "MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/MCPStateSnapshot.h"

# Each check: (label, required literal). The literal is matched exactly so a rename
# or deletion fails the test rather than silently keeping a weaker guard.
BRIDGE_CHECKS = [
    ("unattended guard", "TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true)"),
    ("slow-task guard", "TGuardValue<bool> SilentGuard(GIsSilent, true)"),
    ("modal lookup", "FSlateApplication::Get().GetActiveModalWindow()"),
    ("stall threshold constant", "MCP_EDITOR_STALLED_SECONDS = 15.0"),
    ("refusal keyed on the heartbeat", "StalledSeconds > MCP_EDITOR_STALLED_SECONDS"),
    ("modal error code", 'TEXT("EDITOR_MODAL_ACTIVE")'),
    ("stalled-without-modal code", 'TEXT("EDITOR_BLOCKED")'),
    ("modal recovery hint", "recover_editor to dismiss it"),
    ("recover_editor exempt from modal pre-flight", 'CommandType == TEXT("recover_editor")'),
    ("refusal names the modal class", 'GetModalClass()'),
    ("refusal carries editor state", 'ResponseJson->SetObjectField(TEXT("state"), StateJson);'),
    ("failure keeps handler payload", 'ResponseJson->SetObjectField(TEXT("result"), ResultJson);'),
    ("failure reason fallback", 'TEXT("Command failed without a reason; see \'result\'")'),
]

# Shapes that must NOT come back: the old pre-flight refused on the mere PRESENCE of a window, which
# refused calls while the game thread was demonstrably healthy (observed: 0.035s stall, refused).
BRIDGE_FORBIDDEN = [
    ("modal-presence refusal",
     "if (TSharedPtr<SWindow> ActiveModal = FSlateApplication::Get().GetActiveModalWindow())"),
]

SNAPSHOT_CHECKS = [
    ("stall reader declared", "double GetGameThreadStalledSeconds();"),
    ("modal class reader declared", "FString GetModalClass();"),
    ("modal widget type captured", "Modal->GetContent()->GetType().ToString()"),
    ("modal class written to the snapshot", 'TEXT("modal_class"), ModalClass'),
]

EDITOR_CMD_CHECKS = [
    ("batch success from failures", 'ResultObj->SetBoolField(TEXT("success"), Failures == 0);'),
    ("batch names failed actions", "batch action(s) failed:"),
    ("batch resave sweep", "int32 ResaveStaleReferencers(const TArray<FString>& MovedSources, FString& OutError)"),
    ("sweep reports its count", "still importing a moved path"),
    ("sweep queries stale referencers", "AssetRegistry.GetReferencers(FName(*Source), Referencers)"),
    ("sweep resaves the referencer", "const bool bReSaved = SaveAndVerifyPackage(Loaded ? Loaded->GetOutermost() : nullptr, MovedSources, OutError)"),
]

# The sweep must sit AFTER the batch's last request, never inside the per-request rename step.
BATCH_GATE = "if (Job->Done < Pending->Num()) { return true; }"
BATCH_SWEEP_CALL = "RecordResaveSweep(Job, MovedSources);"


def _missing(root: pathlib.Path) -> list[str]:
    """Return the labels of every guard that is absent from the sources under root."""
    bridge = (root / BRIDGE)
    editor = (root / EDITOR_CMDS)
    snapshot = (root / SNAPSHOT)
    snapshot_h = (root / SNAPSHOT_H)
    bridge_text = bridge.read_text(encoding="utf-8", errors="replace") if bridge.exists() else ""
    editor_text = editor.read_text(encoding="utf-8", errors="replace") if editor.exists() else ""
    snapshot_text = snapshot.read_text(encoding="utf-8", errors="replace") if snapshot.exists() else ""
    snapshot_h_text = snapshot_h.read_text(encoding="utf-8", errors="replace") if snapshot_h.exists() else ""

    missing = [label for label, literal in BRIDGE_CHECKS if literal not in bridge_text]
    missing += [label for label, literal in BRIDGE_FORBIDDEN if literal in bridge_text]
    missing += [label for label, literal in SNAPSHOT_CHECKS if literal not in snapshot_text + snapshot_h_text]
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

    def test_resave_sweep_runs_once_per_batch(self):
        """Both move completion paths sweep, and only after the batch's last request."""
        text = (REPO_ROOT / EDITOR_CMDS).read_text(encoding="utf-8", errors="replace")
        self.assertEqual(text.count(BATCH_GATE), 2, "both move completion paths must gate on the batch")
        self.assertEqual(text.count(BATCH_SWEEP_CALL), 2, "both move completion paths must sweep")
        self.assertLess(text.index(BATCH_GATE), text.index(BATCH_SWEEP_CALL),
                        "the sweep must follow the batch gate, not run inside a request")


if __name__ == "__main__":
    unittest.main(verbosity=2)
