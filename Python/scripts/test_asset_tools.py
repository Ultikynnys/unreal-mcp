#!/usr/bin/env python3
"""Wrapper tests and source-contract checks, not a substitute for Unreal runtime tests."""
import importlib.util
from pathlib import Path
import sys
from types import ModuleType
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
CPP = ROOT / "MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/Commands/UnrealMCPEditorCommands.cpp"

# The tool modules import the shared call helper as `tools.mcp_client`, exactly as the
# server does, so the package root must be importable when a module is loaded by path.
if str(ROOT / "Python") not in sys.path:
    sys.path.insert(0, str(ROOT / "Python"))


class Registry:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def register(function):
            self.tools[function.__name__] = function
            return function
        return register


class AssetToolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("asset_tools_test", ROOT / "Python/tools/asset_tools.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        registry = Registry()
        module.register_asset_tools(registry)
        cls.tools = registry.tools

    def setUp(self):
        self.connection = Mock()
        self.connection.send_command.return_value = {"job_id": "test"}
        self.server = ModuleType("unreal_mcp_server")
        self.server.get_unreal_connection = Mock(return_value=self.connection)
        self.modules = patch.dict(sys.modules, {"unreal_mcp_server": self.server})
        self.modules.start()
        self.addCleanup(self.modules.stop)

    def test_move_defaults_enable_cleanup(self):
        result = self.tools["move_assets"](None, assets=["/Game/Old/Foo"], destination_path="/Game/New")
        self.assertEqual(result, {"job_id": "test"})
        self.connection.send_command.assert_called_once_with("move_assets", {
            "assets": ["/Game/Old/Foo"], "destination_path": "/Game/New",
            "dry_run": False, "fixup_redirectors": True,
        })

    def test_preview_and_opt_out_forwarded(self):
        moves = [{"source": "/Game/A/Foo", "destination": "/Game/B/Bar"}]
        self.tools["move_assets"](None, moves=moves, dry_run=True, fixup_redirectors=False)
        self.connection.send_command.assert_called_once_with("move_assets", {
            "moves": moves, "dry_run": True, "fixup_redirectors": False,
        })

    def test_ambiguous_input_not_dispatched(self):
        result = self.tools["move_assets"](None, moves=[], assets=["/Game/A/Foo"], destination_path="/Game/B")
        self.assertFalse(result["success"])
        self.connection.send_command.assert_not_called()

    def test_empty_input_not_dispatched(self):
        result = self.tools["move_assets"](None)
        self.assertFalse(result["success"])
        self.connection.send_command.assert_not_called()

    def test_folder_defaults(self):
        self.tools["move_folder"](None, "/Game/Old", "/Game/New")
        self.connection.send_command.assert_called_once_with("move_folder", {
            "source_path": "/Game/Old", "destination_path": "/Game/New",
            "recursive": True, "dry_run": False,
        })

    def test_folder_preview_nonrecursive(self):
        self.tools["move_folder"](None, "/Game/Old", "/Game/New", recursive=False, dry_run=True)
        params = self.connection.send_command.call_args.args[1]
        self.assertFalse(params["recursive"])
        self.assertTrue(params["dry_run"])

    def test_connection_failure(self):
        self.server.get_unreal_connection.return_value = None
        self.assertFalse(self.tools["move_folder"](None, "/Game/A", "/Game/B")["success"])
        self.connection.send_command.assert_not_called()

    def test_transport_failure(self):
        self.connection.send_command.side_effect = RuntimeError("disconnected")
        # The shared helper reports failures as '<command> failed: <error>' for every tool.
        self.assertEqual(self.tools["move_folder"](None, "/Game/A", "/Game/B"), {
            "success": False, "message": "move_folder failed: disconnected",
        })


class UnrealSourceContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = CPP.read_text(encoding="utf-8")

    def handler(self, name, next_name):
        start = self.source.index("FUnrealMCPEditorCommands::" + name + "(const")
        end = self.source.index("FUnrealMCPEditorCommands::" + next_name + "(const", start)
        return self.source[start:end]

    def test_preview_and_validation_precede_mutation(self):
        code = self.handler("HandleMoveAssets", "HandleResavePackages")
        create = code.index('CreateMcpJob(TEXT("move_assets")')
        for guard in ("NormalizeAssetPackage(Input.Key", "Sources.Contains(SourceKey)",
                      "Destinations.Contains(DestinationKey)", "PackageOccupied(Destination)",
                      "SourceAssets.Num() != 1", "if (bDryRun) { return Result; }"):
            self.assertLess(code.index(guard), create)

    def test_cleanup_before_next_move(self):
        code = self.handler("HandleMoveAssets", "HandleResavePackages")
        positions = [code.index(text) for text in (
            ".RenameAssets(RenameData)", "SaveLoadedAsset(Asset.Get(), false)",
            "FixAndVerifyRedirector(Request.Source, true, Error)", "*VerificationStarted = FPlatformTime::Seconds()",
        )]
        self.assertEqual(positions, sorted(positions))
        self.assertIn('Job->State = TEXT("failed")', code)
        self.assertIn("FixAndVerifyRedirector(Request.Source, true, Error, false)", code)
        self.assertIn("FPlatformTime::Seconds() - *VerificationStarted < 30.0", code)
        self.assertLess(code.index("if (*VerificationStarted > 0.0)"), code.index("Destination became occupied"))

    def test_folder_mapping_and_guards(self):
        code = self.handler("HandleMoveFolder", "HandleMoveAssets")
        for contract in (
            "Destination + Package.Mid(Source.Len())",
            "Filter.bRecursivePaths = bRecursive",
            'Destination.StartsWith(Source + TEXT("/")',
            'Source.StartsWith(Destination + TEXT("/")',
            "UObjectRedirector::StaticClass()->GetClassPathName()) { continue; }",
            'MoveParams->SetBoolField(TEXT("fixup_redirectors"), true)',
            "return HandleMoveAssets(MoveParams)",
        ):
            self.assertIn(contract, code)

    def test_mutation_handlers_exclude_other_jobs(self):
        for name in ("HandleMoveAssets", "HandleFixupRedirectors", "HandleResavePackages"):
            start = self.source.index("FUnrealMCPEditorCommands::" + name + "(const")
            self.assertIn("if (AssetMutationBusy())", self.source[start:start + 400])

    def test_cleanup_checks_registry_and_disk(self):
        start = self.source.index("    bool FixAndVerifyRedirector(const FString& PackageName, bool bDelete, FString& Error, bool bApplyFixup)\n    {")
        end = self.source.index("FUnrealMCPEditorCommands::HandleMoveFolder", start)
        code = self.source[start:end]
        self.assertIn("TStrongObjectPtr<UObjectRedirector>", code)
        self.assertIn("Remaining.Num() > 0 || FPackageName::DoesPackageExist(PackageName)", code)
        self.assertIn("Referencers.Num() > 0", code)
        self.assertNotIn("FixupReferencers", code)
        self.assertNotIn("ObjectTools::ConsolidateObjects", code)
        self.assertIn("ObjectTools::ForceReplaceReferences(Destination, Olds)", code)
        positions = [code.index(text) for text in (
            "ObjectTools::ForceReplaceReferences(Destination, Olds)",
            "const bool bSavedReferencer = ReferencerWorld",
            "CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS)",
            "ObjectTools::ForceDeleteObjects(EmptyPackage, /*bShowConfirmation=*/false)",
            "AR.GetAssetsByPackageName(FName(*PackageName), Remaining)",
        )]
        self.assertEqual(positions, sorted(positions))
        # A World referencer must not go through the asset save path: that cannot write a map, so
        # the level keeps its old import on disk, which is what broke the maps during the reorg.
        self.assertIn("UWorld::FindWorldInPackage(Loaded)", code)
        self.assertIn("FEditorFileUtils::SaveLevel(ReferencerWorld->PersistentLevel, ReferencerName)", code)
        self.assertIn("Redirector->RemoveFromRoot()", code)
        self.assertIn("AR.WaitForPackage(PackageName)", code)


if __name__ == "__main__":
    unittest.main()
