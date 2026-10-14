#!/usr/bin/env python3
"""Unit tests for build_plugin.py's pure helpers.

The whole point of the script is that the DLL it produces names the current commit, so
the parse of the generated header and the verdict drawn from it are the parts worth
pinning. They run here on fixtures only - no editor, no UBT, and POSIX path shapes, so
the same tests pass on the Linux CI runner as on a Windows checkout.

Run directly or with unittest.
"""

from __future__ import annotations

import importlib.util
import pathlib
import tempfile
import unittest

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("build_plugin", SCRIPT_DIR / "build_plugin.py")
build_plugin = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_plugin)


def _write(path: pathlib.Path, text: str) -> pathlib.Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


class LayoutTests(unittest.TestCase):
    def test_engine_root_is_the_install_above_the_editor(self):
        editor = pathlib.Path("/eng/UE_5.6/Engine/Binaries/Win64/UnrealEditor.exe")
        self.assertEqual(build_plugin.engine_root(editor), pathlib.Path("/eng/UE_5.6"))

    def test_build_bat_sits_under_the_engine(self):
        self.assertEqual(
            build_plugin.build_bat(pathlib.Path("/eng/UE_5.6")),
            pathlib.Path("/eng/UE_5.6/Engine/Build/BatchFiles/Build.bat"))

    def test_definitions_header_names_module_and_target_type(self):
        """The header UBT writes the revision into; both halves of the path must line up."""
        header = build_plugin.definitions_header(pathlib.Path("/p/Plugins/UnrealMCP"))
        self.assertEqual(header.name, "Definitions.UnrealMCP.h")
        self.assertEqual(header.parts[-2], "UnrealMCP")
        self.assertEqual(header.parts[-4], "UnrealEditor")


class ReadBakedRevisionTests(unittest.TestCase):
    def test_reads_revision_and_clean_flag(self):
        with tempfile.TemporaryDirectory() as tmp:
            header = _write(pathlib.Path(tmp) / "Definitions.UnrealMCP.h",
                            '#define MCP_REVISION "abc123"\n#define MCP_REVISION_DIRTY 0\n')
            self.assertEqual(build_plugin.read_baked_revision(header), ("abc123", "0"))

    def test_reads_a_dirty_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            header = _write(pathlib.Path(tmp) / "d.h",
                            '#define MCP_REVISION "deadbeef"\n#define MCP_REVISION_DIRTY 1\n')
            self.assertEqual(build_plugin.read_baked_revision(header), ("deadbeef", "1"))

    def test_missing_header_is_empty_not_an_error(self):
        self.assertEqual(build_plugin.read_baked_revision(pathlib.Path("/nope/d.h")), ("", ""))


class RevisionProblemTests(unittest.TestCase):
    def test_matching_revision_is_clean(self):
        self.assertEqual(build_plugin.revision_problem("abc", "0", "abc"), "")

    def test_a_stale_revision_names_both_sides(self):
        message = build_plugin.revision_problem("old", "0", "new")
        self.assertIn("old", message)
        self.assertIn("new", message)

    def test_no_baked_revision_fails_closed(self):
        self.assertIn("not built", build_plugin.revision_problem("", "", "new"))

    def test_a_dirty_build_is_reported(self):
        self.assertIn("built_dirty", build_plugin.revision_problem("abc", "1", "abc"))


class RefreshTests(unittest.TestCase):
    """The refresh trips UBT's timestamp check and drops the cached rules - and nothing else."""

    def test_rules_cache_paths_are_caches_not_products(self):
        paths = build_plugin.rules_cache_paths(pathlib.Path("/p"))
        self.assertEqual([path.name for path in paths], ["BuildRules", "SourceFileCache.bin"])

    def test_build_cs_points_at_the_rule_file(self):
        rules = build_plugin.build_cs(pathlib.Path("/p/Plugins/UnrealMCP"))
        self.assertEqual(rules,
                         pathlib.Path("/p/Plugins/UnrealMCP/Source/UnrealMCP/UnrealMCP.Build.cs"))

    def test_refresh_touches_the_rule_file_without_changing_it(self):
        """Touching, not editing: an edit would make the module tree dirty and set built_dirty."""
        with tempfile.TemporaryDirectory() as tmp:
            project = pathlib.Path(tmp)
            plugin = project / "Plugins" / "UnrealMCP"
            rules = build_plugin.build_cs(plugin)
            _write(rules, "public class UnrealMCP {}\n")
            before = rules.read_text(encoding="utf-8")
            done = build_plugin.refresh_build_state(plugin, project)
            self.assertEqual(rules.read_text(encoding="utf-8"), before)
            self.assertTrue(any("touched" in action for action in done))

    def test_refresh_leaves_build_products_alone(self):
        """A refused build must not leave the plugin unbuilt, so the products stay put."""
        with tempfile.TemporaryDirectory() as tmp:
            project = pathlib.Path(tmp)
            plugin = project / "Plugins" / "UnrealMCP"
            (plugin / "Binaries" / "Win64").mkdir(parents=True)
            keep = plugin / "Binaries" / "Win64" / "UnrealEditor-UnrealMCP.dll"
            keep.write_bytes(b"dll")
            build_plugin.refresh_build_state(plugin, project)
            self.assertTrue(keep.is_file(), "the refresh deleted a build product")


class ResolveInstallTests(unittest.TestCase):
    def test_a_host_project_folder(self):
        with tempfile.TemporaryDirectory() as tmp:
            project = pathlib.Path(tmp) / "gmtk2026"
            (project / "Plugins").mkdir(parents=True)
            _write(project / "Haeretica.uproject", "{}")
            plugin, uproject = build_plugin.resolve_install(project)
            self.assertEqual(plugin, project / "Plugins" / "UnrealMCP")
            self.assertEqual(uproject, project / "Haeretica.uproject")

    def test_a_uproject_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            project = pathlib.Path(tmp) / "P"
            _write(project / "P.uproject", "{}")
            plugin, uproject = build_plugin.resolve_install(project / "P.uproject")
            self.assertEqual(plugin, project / "Plugins" / "UnrealMCP")
            self.assertEqual(uproject, project / "P.uproject")

    def test_the_plugin_folder_itself(self):
        with tempfile.TemporaryDirectory() as tmp:
            project = pathlib.Path(tmp) / "P"
            plugin_dir = project / "Plugins" / "UnrealMCP"
            plugin_dir.mkdir(parents=True)
            _write(project / "P.uproject", "{}")
            plugin, uproject = build_plugin.resolve_install(plugin_dir)
            self.assertEqual(plugin, plugin_dir)
            self.assertEqual(uproject, project / "P.uproject")

    def test_an_ambiguous_folder_has_no_uproject(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = pathlib.Path(tmp) / "not-a-project"
            folder.mkdir()
            _write(folder / "A.uproject", "{}")
            _write(folder / "B.uproject", "{}")
            _plugin, uproject = build_plugin.resolve_install(folder)
            self.assertIsNone(uproject, "two .uproject files must not be guessed")


if __name__ == "__main__":
    unittest.main(verbosity=2)
