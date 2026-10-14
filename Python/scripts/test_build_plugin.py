#!/usr/bin/env python3
"""Unit tests for build_plugin.py's pure helpers.

The script exists to build a plugin whose version matches what the server speaks, so the
two version parses and the verdict drawn from them are the parts worth pinning. They run
here on fixtures only - no editor, no UBT, and POSIX path shapes, so the same tests pass
on the Linux CI runner as on a Windows checkout.

Run directly or with unittest.
"""

from __future__ import annotations

import importlib.util
import pathlib
import tempfile
import unittest
import zipfile

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


class VersionTests(unittest.TestCase):
    def test_reads_the_version_from_a_plugin_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            uplugin = _write(pathlib.Path(tmp) / "UnrealMCP.uplugin",
                             '{\n\t"VersionName": "1.4.0"\n}\n')
            self.assertEqual(build_plugin.plugin_version(uplugin), "1.4.0")

    def test_a_manifest_without_a_version_is_empty(self):
        with tempfile.TemporaryDirectory() as tmp:
            uplugin = _write(pathlib.Path(tmp) / "U.uplugin", "{}\n")
            self.assertEqual(build_plugin.plugin_version(uplugin), "")

    def test_a_missing_manifest_is_empty_not_an_error(self):
        self.assertEqual(build_plugin.plugin_version(pathlib.Path("/nope/U.uplugin")), "")

    def test_reads_the_server_plugin_version(self):
        with tempfile.TemporaryDirectory() as tmp:
            server = _write(pathlib.Path(tmp) / "unreal_mcp_server.py",
                            'PLUGIN_VERSION = "1.4.0"\nother = 1\n')
            self.assertEqual(build_plugin.server_version(server), "1.4.0")

    def test_agreement_is_clean(self):
        self.assertEqual(build_plugin.version_problem("1.4.0", "1.4.0"), "")

    def test_a_mismatch_names_both_sides(self):
        message = build_plugin.version_problem("1.3.0", "1.4.0")
        self.assertIn("1.3.0", message)
        self.assertIn("1.4.0", message)

    def test_either_side_missing_fails_closed(self):
        self.assertIn("PLUGIN_VERSION", build_plugin.version_problem("1.4.0", ""))
        self.assertIn("VersionName", build_plugin.version_problem("", "1.4.0"))


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


class TransformTests(unittest.TestCase):
    BUILD_CS = (
        "using UnrealBuildTool;\n"
        "\n"
        "public class UnrealMCP : ModuleRules\n"
        "{\n"
        "\tpublic UnrealMCP(ReadOnlyTargetRules Target) : base(Target)\n"
        "\t{\n"
        "\t\tPCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;\n"
        "\t\tIWYUSupport = IWYUSupport.Full;\n"
        "\t}\n"
        "}\n"
    )

    def test_precompiled_line_is_added_after_pchusage(self):
        out = build_plugin.with_precompiled(self.BUILD_CS)
        self.assertIn("\t\tPCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;\n"
                      "\t\tbUsePrecompiled = true;\n", out)

    def test_precompiled_leaves_the_rest_untouched(self):
        out = build_plugin.with_precompiled(self.BUILD_CS)
        self.assertEqual(out.replace("\t\tbUsePrecompiled = true;\n", ""), self.BUILD_CS)

    def test_precompiled_is_idempotent(self):
        once = build_plugin.with_precompiled(self.BUILD_CS)
        self.assertEqual(build_plugin.with_precompiled(once), once)

    def test_installed_true_flips_false(self):
        manifest = '{\n\t"Installed": false,\n\t"VersionName": "1.4.0"\n}\n'
        flipped = build_plugin.with_installed_true(manifest)
        self.assertIn('"Installed": true', flipped)
        self.assertNotIn('"Installed": false', flipped)

    def test_installed_true_is_added_when_absent(self):
        manifest = '{\n\t"FileVersion": 3,\n\t"VersionName": "1.4.0"\n}\n'
        self.assertIn('"Installed": true', build_plugin.with_installed_true(manifest))


class PackageNameTests(unittest.TestCase):
    def test_engine_tag_from_the_install_folder(self):
        self.assertEqual(build_plugin.engine_tag(pathlib.Path("/eng/UE_5.6")), "5.6")
        self.assertEqual(build_plugin.engine_tag(pathlib.Path("/eng/not-an-engine")), "")

    def test_the_asset_name_matches_the_splinearray_form(self):
        self.assertEqual(build_plugin.package_name("1.4.0", "5.6"),
                         "UnrealMCP-1.4.0-UE5.6-Win64.zip")

    def test_debug_symbols_are_not_shipped(self):
        self.assertFalse(build_plugin.is_shipped(pathlib.Path("Binaries/Win64/x.pdb")))
        self.assertTrue(build_plugin.is_shipped(pathlib.Path("Binaries/Win64/x.dll")))
        self.assertTrue(build_plugin.is_shipped(pathlib.Path("Binaries/Win64/UnrealEditor.modules")))


class BinaryFormTests(unittest.TestCase):
    """A host copy carries no C++, a bUsePrecompiled stub, and an Installed:true manifest."""

    @staticmethod
    def _fake_plugin(root: pathlib.Path) -> pathlib.Path:
        plugin = root / "UnrealMCP"
        _write(plugin / "UnrealMCP.uplugin",
               '{\n\t"Installed": false,\n\t"VersionName": "1.4.0"\n}\n')
        _write(plugin / "Source" / "UnrealMCP" / "UnrealMCP.Build.cs", TransformTests.BUILD_CS)
        _write(plugin / "Source" / "UnrealMCP" / "Private" / "UnrealMCPBridge.cpp", "// cpp\n")
        _write(plugin / "Source" / "UnrealMCP" / "Public" / "MCPPluginVersion.h", "// h\n")
        _write(plugin / "Binaries" / "Win64" / "UnrealEditor-UnrealMCP.dll", "dll")
        _write(plugin / "Binaries" / "Win64" / "UnrealEditor-UnrealMCP.pdb", "debug symbols")
        _write(plugin / "Binaries" / "Win64" / "UnrealEditor.modules", "{}")
        return plugin

    def test_install_writes_the_binary_form_and_no_cpp(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            dest = root / "host" / "Plugins" / "UnrealMCP"
            build_plugin.binary_form(self._fake_plugin(root), dest)
            self.assertIn('"Installed": true',
                          (dest / "UnrealMCP.uplugin").read_text(encoding="utf-8"))
            stub = (dest / "Source" / "UnrealMCP" / "UnrealMCP.Build.cs").read_text(encoding="utf-8")
            self.assertIn("bUsePrecompiled = true;", stub)
            self.assertTrue((dest / "Binaries" / "Win64" / "UnrealEditor-UnrealMCP.dll").is_file())
            cpp = [p.name for p in dest.rglob("*.[ch]pp")] + [p.name for p in dest.rglob("*.h")]
            self.assertEqual(cpp, [], f"the host copy must carry no C++ source: {cpp}")
            self.assertEqual(list(dest.rglob("*.pdb")), [], "debug symbols must not ship to the host")

    def test_install_clears_a_previous_source_copy(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            dest = root / "host" / "Plugins" / "UnrealMCP"
            _write(dest / "Source" / "UnrealMCP" / "Private" / "Stale.cpp", "// stale\n")
            build_plugin.binary_form(self._fake_plugin(root), dest)
            self.assertEqual(list(dest.rglob("*.cpp")), [])

    def test_package_layout_is_the_binary_form(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            out = root / "dist" / "UnrealMCP-1.4.0-UE5.6-Win64.zip"
            build_plugin.package_plugin(self._fake_plugin(root), out)
            with zipfile.ZipFile(out) as archive:
                names = sorted(archive.namelist())
            self.assertEqual(names, ["Binaries/Win64/UnrealEditor-UnrealMCP.dll",
                                     "Binaries/Win64/UnrealEditor.modules",
                                     "Source/UnrealMCP/UnrealMCP.Build.cs",
                                     "UnrealMCP.uplugin"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
