#!/usr/bin/env python3
"""Build the UnrealMCP plugin and (optionally) install it into a host project.

The plugin's identity is its own version (UnrealMCP.uplugin VersionName), which the bridge reports
at runtime from its descriptor and the server checks, so a plugin build is portable across projects
and a commit no longer makes an installed DLL stale. This builds that plugin from the repo, checks
that the repo plugin and the server agree on the version, and can install it into another project
and bring that editor back.

    python Python/scripts/build_plugin.py                      # reap, build, verify
    python Python/scripts/build_plugin.py --restart             # ... then bring the editor back
    python Python/scripts/build_plugin.py --install-into DIR    # copy the plugin into another project
    python Python/scripts/build_plugin.py --install-into DIR --restart

`--install-into` takes a host project folder, its .uproject, or the plugin folder. The editor
holding the target's DLL (and any editor whose Live Coding would block UBT) is reaped first; a
stray one is named on failure (`--all-editors` reaps it too). `--force` additionally drops UBT's
cached rules and touches the rule file, the escape hatch when a build answers "Target is up to
date" and should not.

Exit code is non-zero when the build fails or the repo plugin and the server disagree.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import zipfile

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[1]
# reap/restart/bridge helpers already live with the rest of the editor lifecycle.
sys.path.insert(0, str(SCRIPT_DIR))
import editor_process as ep

MODULE = "UnrealMCP"
BUILD_CS_REL = ("Source", MODULE, f"{MODULE}.Build.cs")
# UBT refuses to build while a Live Coding session is up, and it does not name the editor.
LIVE_CODING_MARKER = "Live Coding is active"
DEFAULT_UPROJECT = REPO_ROOT / "MCPGameProject" / "MCPGameProject.uproject"
DEFAULT_TARGET = "MCPGameProjectEditor"
DEFAULT_PLATFORM = "Win64"
DEFAULT_CONFIG = "Development"


# ---------------------------------------------------------------------------
# Pure helpers (unit-tested in test_build_plugin.py, no editor and no Windows)
# ---------------------------------------------------------------------------


def engine_root(editor_exe: pathlib.Path) -> pathlib.Path:
    """`.../UE_5.6/Engine/Binaries/Win64/UnrealEditor.exe` -> `.../UE_5.6`."""
    return editor_exe.parents[3]


def build_bat(engine: pathlib.Path) -> pathlib.Path:
    """The UBT launcher inside an engine install."""
    return engine / "Engine" / "Build" / "BatchFiles" / "Build.bat"


def plugin_version(uplugin: pathlib.Path) -> str:
    """The version in a plugin's own manifest; '' when it has none."""
    try:
        text = uplugin.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""
    match = re.search(r'"VersionName"\s*:\s*"([^"]*)"', text)
    return match.group(1) if match else ""


def server_version(server_py: pathlib.Path) -> str:
    """The plugin version the server declares it speaks (its PLUGIN_VERSION literal)."""
    try:
        text = server_py.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""
    match = re.search(r'^PLUGIN_VERSION\s*=\s*"([^"]*)"', text, re.MULTILINE)
    return match.group(1) if match else ""


def build_cs(plugin_dir: pathlib.Path) -> pathlib.Path:
    """The rule file UBT re-evaluates when it looks newer than the last build's output."""
    return plugin_dir.joinpath(*BUILD_CS_REL)


def rules_cache_paths(project_dir: pathlib.Path) -> list[pathlib.Path]:
    """UBT's compiled rules and source scan; deleting them re-runs BuildRevision().

    Neither is a build product, so removing them cannot leave the plugin unbuilt.
    """
    build = project_dir / "Intermediate" / "Build"
    return [build / "BuildRules", build / "SourceFileCache.bin"]


def version_problem(plugin: str, server: str) -> str:
    """'' when the plugin and the server speak the same version, otherwise why not."""
    if not server:
        return "cannot read the server's PLUGIN_VERSION"
    if not plugin:
        return "cannot read the plugin's VersionName (no UnrealMCP.uplugin?)"
    if plugin != server:
        return f"the plugin is version {plugin} but the server speaks {server}"
    return ""


def with_installed_true(manifest: str) -> str:
    """The plugin manifest as a host consumes it: `"Installed": true`.

    UE then loads the plugin from Binaries/ instead of expecting to compile source, which is
    what lets a host project carry a plugin with no C++ in it (the SplineArray pattern).
    """
    if re.search(r'"Installed"\s*:\s*(true|false)', manifest):
        return re.sub(r'"Installed"\s*:\s*(true|false)', '"Installed": true', manifest, count=1)
    return re.sub(r'(?m)^(\s*)"FileVersion".*\n',
                  lambda m: m.group(0) + '\t"Installed": true,\n', manifest, count=1)


def with_precompiled(build_cs_text: str) -> str:
    """The module rules with `bUsePrecompiled = true;` added, so UBT links the shipped binary.

    The repo's file is otherwise kept byte-for-byte, so the module's dependency list travels
    with the binary; only this one line is added, right after PCHUsage.
    """
    if "bUsePrecompiled" in build_cs_text:
        return build_cs_text
    return re.sub(r"(?m)^([ \t]*)PCHUsage\s*=.*\n",
                  lambda m: m.group(0) + m.group(1) + "bUsePrecompiled = true;\n",
                  build_cs_text, count=1)


def engine_tag(engine: pathlib.Path) -> str:
    """The engine version from an install folder name: `.../UE_5.6` -> `5.6`."""
    match = re.search(r"UE_([0-9]+\.[0-9]+)", engine.name)
    return match.group(1) if match else ""


def package_name(version: str, tag: str) -> str:
    """The release asset name, matching the SplineArray form.

    `<Module>-<version>-UE<engine>-<platform>.zip`, e.g. UnrealMCP-1.4.0-UE5.6-Win64.zip.
    """
    return f"{MODULE}-{version}-UE{tag}-{DEFAULT_PLATFORM}.zip"


def find_uproject(project_dir: pathlib.Path) -> pathlib.Path | None:
    """The single .uproject in a project folder, or None when it is not one."""
    matches = sorted(project_dir.glob("*.uproject"))
    return matches[0] if len(matches) == 1 else None


def resolve_install(value: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path | None]:
    """(plugin folder, host .uproject) for an --install-into value.

    Takes a host project folder, its .uproject, or the plugin folder itself.
    """
    if value.suffix.lower() == ".uproject":
        return value.parent / "Plugins" / MODULE, value
    if value.name == MODULE:
        return value, find_uproject(value.parents[1])
    return value / "Plugins" / MODULE, find_uproject(value)


# ---------------------------------------------------------------------------
# OS-touching helpers
# ---------------------------------------------------------------------------


def reap(uproject: pathlib.Path, every: bool = False) -> None:
    """Kill this project's editors (every editor with `every`) so nothing holds the DLL."""
    ep.cmd_reap(argparse.Namespace(uproject=uproject, all=every, orphans_only=False))


def running_editors(uproject: pathlib.Path) -> list[dict]:
    """Every editor process, so a Live Coding refusal can name the one holding it."""
    processes = ep.list_unreal_processes()
    classified = ep.classify_processes(processes, uproject, {p["pid"] for p in processes})
    return [p for p in classified if p["kind"] == "editor"]


def drop(paths: list[pathlib.Path]) -> list[str]:
    removed = []
    for path in paths:
        if not path.exists():
            continue
        if path.is_dir():
            shutil.rmtree(path, ignore_errors=True)
        else:
            path.unlink(missing_ok=True)
        removed.append(str(path))
    return removed


def refresh_build_state(plugin_dir: pathlib.Path, project_dir: pathlib.Path) -> list[str]:
    """Make UBT rebuild and re-run BuildRevision() without touching a build product.

    The rules cache is deleted; the rule file is then touched so it looks newer than the DLL
    the last build linked. Touching rather than editing keeps `git status` empty, which is
    what holds `built_dirty` at 0 (an edit, even whitespace, would set it).
    """
    done = drop(rules_cache_paths(project_dir))
    rules = build_cs(plugin_dir)
    if rules.exists():
        os.utime(rules, None)
        done.append(f"touched {rules.name} so UBT re-evaluates the rules")
    return done


def run_build(engine: pathlib.Path, uproject: pathlib.Path, target: str,
              platform: str, config: str) -> tuple[int, str]:
    """Run UBT; return (exit code, combined output). 127 when the launcher is missing."""
    launcher = build_bat(engine)
    if not launcher.exists():
        return 127, f"build launcher not found: {launcher} (pass --engine)"
    command = [str(launcher), target, platform, config, f"-Project={uproject}", "-WaitMutex"]
    print("build: " + " ".join(command))
    done = subprocess.run(command, capture_output=True, text=True)
    output = (done.stdout or "") + (done.stderr or "")
    print("\n".join(output.strip().splitlines()[-12:]))
    return done.returncode, output




def binary_form(plugin_dir: pathlib.Path, dest: pathlib.Path) -> None:
    """Install the plugin into a host as BINARIES ONLY - the SplineArray pattern.

    Writes the manifest with `"Installed": true`, a `Source/<Module>/<Module>.Build.cs` stub that
    carries the dependency list plus `bUsePrecompiled = true` (so UBT links the shipped DLL), and
    `Binaries/` - and no C++. The host's Source/<Module> is cleared first, so a stray Private/
    Public from an earlier source install cannot linger.
    """
    dest.mkdir(parents=True, exist_ok=True)
    module_dir = dest / "Source" / MODULE
    shutil.rmtree(module_dir, ignore_errors=True)
    module_dir.mkdir(parents=True, exist_ok=True)

    stub = with_precompiled(build_cs(plugin_dir).read_text(encoding="utf-8"))
    (module_dir / f"{MODULE}.Build.cs").write_text(stub, encoding="utf-8")
    print(f"install: Source/{MODULE}/{MODULE}.Build.cs (bUsePrecompiled stub, no C++)")

    manifest = (plugin_dir / f"{MODULE}.uplugin").read_text(encoding="utf-8")
    (dest / f"{MODULE}.uplugin").write_text(with_installed_true(manifest), encoding="utf-8")
    print(f"install: {MODULE}.uplugin (Installed: true)")

    shutil.rmtree(dest / "Binaries", ignore_errors=True)
    shutil.copytree(plugin_dir / "Binaries", dest / "Binaries")
    print(f"install: Binaries -> {dest / 'Binaries'}")


def package_plugin(plugin_dir: pathlib.Path, out_zip: pathlib.Path) -> None:
    """Write the binary-only plugin zip a host unzips into `Plugins/<Module>`.

    The entries are exactly what binary_form() installs: the Installed:true manifest, the
    bUsePrecompiled Build.cs stub, and Binaries/ - nothing else, so no C++ ships.
    """
    out_zip.parent.mkdir(parents=True, exist_ok=True)
    manifest = (plugin_dir / f"{MODULE}.uplugin").read_text(encoding="utf-8")
    stub = build_cs(plugin_dir).read_text(encoding="utf-8")
    with zipfile.ZipFile(out_zip, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(f"{MODULE}.uplugin", with_installed_true(manifest))
        archive.writestr(f"Source/{MODULE}/{MODULE}.Build.cs", with_precompiled(stub))
        for path in sorted((plugin_dir / "Binaries").rglob("*")):
            if path.is_file():
                archive.write(path, str(path.relative_to(plugin_dir)).replace("\\", "/"))
    print(f"package: {out_zip} ({out_zip.stat().st_size} bytes)")


def upload_release(tag: str, asset: pathlib.Path) -> int:
    """Attach the packaged zip to a GitHub release with gh (`--clobber` to replace it)."""
    try:
        done = subprocess.run(["gh", "release", "upload", tag, str(asset), "--clobber"],
                              capture_output=True, text=True, timeout=180)
    except (OSError, subprocess.SubprocessError) as error:
        print(f"upload: failed: {error}")
        return 1
    print(((done.stdout or "") + (done.stderr or "")).strip())
    return done.returncode


def ping_revision(timeout: float = 2.0) -> dict:
    """The bridge's ping reply (carrying its `plugin_version`), or {} when nothing answers."""
    secret = ep.control_plane_secret(ep.SERVER_PY)
    payload = json.dumps({"type": "ping", "params": {}, "access_key": secret})
    try:
        with socket.create_connection((ep.HOST, ep.PORT), timeout=timeout) as sock:
            sock.settimeout(timeout)
            sock.sendall(payload.encode("utf-8"))
            chunks = []
            while True:
                chunk = sock.recv(65536)
                if not chunk:
                    break
                chunks.append(chunk)
                try:
                    return json.loads(b"".join(chunks).decode("utf-8"))
                except json.JSONDecodeError:
                    continue
    except (OSError, socket.timeout):
        return {}
    return {}


def wait_for_handshake(wait: float, version: str) -> bool:
    """Poll until the relaunched editor answers with the plugin version we expect."""
    deadline = time.time() + wait
    while time.time() < deadline:
        reply = ping_revision()
        reported = str(reply.get("plugin_version") or "")
        if reported == version:
            print(f"handshake: the editor answers with plugin version {reported}")
            return True
        if reported:
            print(f"handshake: the editor answers with {reported}, waiting for {version}")
        time.sleep(2.0)
    print(f"handshake: no matching version within {wait:.0f}s")
    return False


# ---------------------------------------------------------------------------
# Command
# ---------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--uproject", type=pathlib.Path, default=DEFAULT_UPROJECT)
    parser.add_argument("--engine", type=pathlib.Path, default=engine_root(ep.DEFAULT_EDITOR))
    parser.add_argument("--editor", type=pathlib.Path, default=ep.DEFAULT_EDITOR)
    parser.add_argument("--target", default=DEFAULT_TARGET)
    parser.add_argument("--platform", default=DEFAULT_PLATFORM)
    parser.add_argument("--config", default=DEFAULT_CONFIG)
    parser.add_argument("--install-into", type=pathlib.Path,
                        help="host project (folder, .uproject, or plugin folder) to install into")
    parser.add_argument("--package", type=pathlib.Path, metavar="DIR",
                        help="write the binary-only release zip into DIR "
                             "(UnrealMCP-<ver>-UE<engine>-Win64.zip)")
    parser.add_argument("--upload", metavar="TAG",
                        help="after packaging, upload the zip to that GitHub release tag (uses gh)")
    parser.add_argument("--restart", action="store_true",
                        help="launch the (installed) editor and wait for the handshake")
    parser.add_argument("--wait", type=float, default=180.0,
                        help="seconds to wait for the handshake after --restart")
    parser.add_argument("--force", action="store_true",
                        help="drop UBT's cached rules and touch the rule file first (only needed "
                             "when a build answers 'Target is up to date')")
    parser.add_argument("--all-editors", action="store_true",
                        help="reap every editor, not just this project's and the target's")
    args = parser.parse_args(argv)

    project_dir = args.uproject.parent
    plugin_dir = project_dir / "Plugins" / MODULE

    expected = server_version(ep.SERVER_PY)
    if not expected:
        print(f"abort: cannot read PLUGIN_VERSION from {ep.SERVER_PY}")
        return 2
    print(f"server speaks UnrealMCP {expected}")

    dest_plugin = dest_uproject = None
    if args.install_into:
        dest_plugin, dest_uproject = resolve_install(args.install_into)
        print(f"install target: {dest_plugin}")

    # A running editor holds its project's DLL, and any editor's Live Coding blocks the build.
    for uproject in (args.uproject, dest_uproject):
        if uproject is not None:
            reap(uproject, every=args.all_editors)

    if args.force:
        for action in refresh_build_state(plugin_dir, project_dir):
            print(f"refresh: {action}")

    code, output = run_build(args.engine, args.uproject, args.target, args.platform, args.config)
    if code != 0:
        print(f"build: FAILED (exit {code})")
        if LIVE_CODING_MARKER in output:
            print("build: an editor holds Live Coding on this engine; UBT cannot build around it.")
            for editor in running_editors(args.uproject):
                print(f"build:   pid={editor['pid']} {editor['name']} {editor['command_line']}")
            print("build: close it, or rerun with --all-editors to reap every editor")
        return code

    version = plugin_version(plugin_dir / f"{MODULE}.uplugin")
    problem = version_problem(version, expected)
    print(f"verify: plugin={version or '?'} server={expected}")
    if problem:
        print(f"verify: FAILED - {problem}")
        return 1
    print(f"verify: the plugin and the server agree on UnrealMCP {version}")

    if dest_plugin is not None:
        binary_form(plugin_dir, dest_plugin)

    if args.package or args.upload:
        out_dir = args.package or pathlib.Path(tempfile.mkdtemp(prefix="unrealmcp-package-"))
        out_zip = out_dir / package_name(version, engine_tag(args.engine))
        package_plugin(plugin_dir, out_zip)
        if args.upload:
            code = upload_release(args.upload, out_zip)
            if code != 0:
                return code

    if args.restart:
        if dest_plugin is not None and dest_uproject is None:
            print("restart: could not find the host .uproject to launch")
            return 1
        uproject = dest_uproject or args.uproject
        ep.launch_editor(args.editor, uproject)
        print(f"restart: launched {args.editor.name} on {uproject.name}")
        return 0 if wait_for_handshake(args.wait, version) else 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
