#!/usr/bin/env python3
"""Rebuild the UnrealMCP plugin so a running editor actually picks the change up.

Committing moves the server's revision the moment it lands, and the plugin bakes the
commit it was BUILT from, so after any commit every call is refused until the DLL is
rebuilt. Two traps make that rebuild lie, and this script exists to close both:

  * `Build.bat` answers "Target is up to date" and keeps the old revision, because its
    up-to-date check is a timestamp comparison and a just-linked DLL is already newer
    than the sources the commit touched. Touching `UnrealMCP.Build.cs` (mtime only) puts a
    newer input back in front of that check and forces the rebuild;
  * even then UBT reuses the compiled module rules, so `MCP_REVISION` still names the
    previous commit. Deleting the project's BuildRules re-runs `BuildRevision()`.

It refreshes from a CLEAN tree: the rule file is touched, never edited, so `git status`
stays empty and the DLL reports `built_dirty: false` instead of carrying a whitespace
edit's dirty tree as a warning. Only UBT's caches are deleted, so a build that is refused
leaves the plugin exactly as it was.

A third trap stops the build outright. UBT refuses while any editor's Live Coding session
is up, and it does not name the editor: this project's and the `--install-into` host's are
reaped first, and a stray one is named on failure (`--all-editors` reaps it too).

    python Python/scripts/build_plugin.py                     # reap, rebuild, verify
    python Python/scripts/build_plugin.py --restart            # ... then bring the editor back
    python Python/scripts/build_plugin.py --install-into DIR   # copy the plugin into another project
    python Python/scripts/build_plugin.py --install-into DIR --restart

`--install-into` is for a plugin copied into another project: the copy must carry THIS
repo's revision, which is exactly what the build above bakes (building the copy in place
would record the host's HEAD instead). The editor holding the target's DLL is reaped
before the copy, and `--restart` relaunches it and asserts the handshake.

Exit code is non-zero when the build fails or the baked revision does not name HEAD.
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
import time

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[1]
# reap/restart/bridge helpers already live with the rest of the editor lifecycle.
sys.path.insert(0, str(SCRIPT_DIR))
import editor_process as ep

MODULE = "UnrealMCP"
BUILD_CS_REL = ("Source", MODULE, f"{MODULE}.Build.cs")
INSTALL_PARTS = ("Binaries", "Source", "Content", f"{MODULE}.uplugin")
# UBT refuses to build while a Live Coding session is up, and it does not name the editor.
LIVE_CODING_MARKER = "Live Coding is active"
DEFAULT_UPROJECT = REPO_ROOT / "MCPGameProject" / "MCPGameProject.uproject"
DEFAULT_TARGET = "MCPGameProjectEditor"
DEFAULT_PLATFORM = "Win64"
DEFAULT_CONFIG = "Development"
# TargetType.Editor compiles to <Platform>/<Arch>/UnrealEditor/<Config>/<Module>.
TARGET_TYPE_DIR = "UnrealEditor"
ARCH = "x64"


# ---------------------------------------------------------------------------
# Pure helpers (unit-tested in test_build_plugin.py, no editor and no Windows)
# ---------------------------------------------------------------------------


def engine_root(editor_exe: pathlib.Path) -> pathlib.Path:
    """`.../UE_5.6/Engine/Binaries/Win64/UnrealEditor.exe` -> `.../UE_5.6`."""
    return editor_exe.parents[3]


def build_bat(engine: pathlib.Path) -> pathlib.Path:
    """The UBT launcher inside an engine install."""
    return engine / "Engine" / "Build" / "BatchFiles" / "Build.bat"


def definitions_header(plugin_dir: pathlib.Path, config: str = DEFAULT_CONFIG) -> pathlib.Path:
    """The generated header UBT writes MCP_REVISION into for this module's build."""
    return (plugin_dir / "Intermediate" / "Build" / DEFAULT_PLATFORM / ARCH / TARGET_TYPE_DIR
            / config / MODULE / f"Definitions.{MODULE}.h")


def read_baked_revision(header: pathlib.Path) -> tuple[str, str]:
    """(MCP_REVISION, MCP_REVISION_DIRTY) from the header; ('', '') when it is missing."""
    try:
        text = header.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return "", ""
    revision = re.search(r'#define\s+MCP_REVISION\s+"([^"]*)"', text)
    dirty = re.search(r"#define\s+MCP_REVISION_DIRTY\s+([01])", text)
    return (revision.group(1) if revision else ""), (dirty.group(1) if dirty else "")


def build_cs(plugin_dir: pathlib.Path) -> pathlib.Path:
    """The rule file UBT re-evaluates when it looks newer than the last build's output."""
    return plugin_dir.joinpath(*BUILD_CS_REL)


def rules_cache_paths(project_dir: pathlib.Path) -> list[pathlib.Path]:
    """UBT's compiled rules and source scan; deleting them re-runs BuildRevision().

    Neither is a build product, so removing them cannot leave the plugin unbuilt.
    """
    build = project_dir / "Intermediate" / "Build"
    return [build / "BuildRules", build / "SourceFileCache.bin"]


def revision_problem(baked: str, dirty: str, head: str) -> str:
    """'' when the DLL identifies the current commit, otherwise why it does not."""
    if not baked:
        return f"no baked revision: {MODULE} was not built (no Definitions.{MODULE}.h)"
    if baked != head:
        return f"the DLL bakes {baked[:9]} but HEAD is {head[:9]}"
    if dirty != "0":
        return "built_dirty is set: the module tree was dirty at build time"
    return ""


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


def head_revision(repo_root: pathlib.Path) -> str:
    try:
        done = subprocess.run(["git", "-C", str(repo_root), "rev-parse", "HEAD"],
                              capture_output=True, text=True, timeout=15)
    except (OSError, subprocess.SubprocessError):
        return ""
    return done.stdout.strip() if done.returncode == 0 else ""


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


def install_plugin(plugin_dir: pathlib.Path, dest: pathlib.Path) -> None:
    """Copy the plugin's binaries and source over a host project's copy.

    Delete-then-copy per part, so a part the source no longer has cannot linger.
    """
    dest.mkdir(parents=True, exist_ok=True)
    for name in INSTALL_PARTS:
        source = plugin_dir / name
        if not source.exists():
            continue
        target = dest / name
        if target.is_dir():
            shutil.rmtree(target, ignore_errors=True)
        elif target.exists():
            target.unlink()
        if source.is_dir():
            shutil.copytree(source, target)
        else:
            shutil.copy2(source, target)
        print(f"install: {name} -> {target}")


def ping_revision(timeout: float = 2.0) -> dict:
    """The bridge's ping reply (carrying its own `revision`), or {} when nothing answers."""
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


def wait_for_handshake(wait: float, head: str) -> bool:
    """Poll until the relaunched editor answers with the revision we just baked."""
    deadline = time.time() + wait
    while time.time() < deadline:
        reply = ping_revision()
        reported = str(reply.get("revision") or "")
        if reported == head:
            print(f"handshake: the editor answers with {reported[:9]} (matches HEAD)")
            return True
        if reported:
            print(f"handshake: the editor answers with {reported[:9]}, waiting for {head[:9]}")
        time.sleep(2.0)
    print(f"handshake: no matching revision within {wait:.0f}s")
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
                        help="host project (folder, .uproject, or plugin folder) to copy into")
    parser.add_argument("--restart", action="store_true",
                        help="launch the (installed) editor and wait for the handshake")
    parser.add_argument("--wait", type=float, default=180.0,
                        help="seconds to wait for the handshake after --restart")
    parser.add_argument("--skip-refresh", action="store_true",
                        help="do not touch the rules cache or the rule file (fast; may keep a stale revision)")
    parser.add_argument("--all-editors", action="store_true",
                        help="reap every editor, not just this project's and the target's")
    args = parser.parse_args(argv)

    project_dir = args.uproject.parent
    plugin_dir = project_dir / "Plugins" / MODULE

    head = head_revision(REPO_ROOT)
    if not head:
        print("abort: cannot read HEAD (is this a git checkout?)")
        return 2
    print(f"repo HEAD: {head}")

    dest_plugin = dest_uproject = None
    if args.install_into:
        dest_plugin, dest_uproject = resolve_install(args.install_into)
        print(f"install target: {dest_plugin}")

    # A running editor holds its project's DLL, and any editor's Live Coding blocks the build.
    for uproject in (args.uproject, dest_uproject):
        if uproject is not None:
            reap(uproject, every=args.all_editors)

    if not args.skip_refresh:
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

    baked, dirty = read_baked_revision(definitions_header(plugin_dir, args.config))
    problem = revision_problem(baked, dirty, head)
    print(f"verify: baked={baked[:9] or '?'} dirty={dirty or '?'} head={head[:9]}")
    if problem:
        print(f"verify: FAILED - {problem}")
        return 1
    print("verify: the DLL identifies the current commit")

    if dest_plugin is not None:
        install_plugin(plugin_dir, dest_plugin)

    if args.restart:
        if dest_plugin is not None and dest_uproject is None:
            print("restart: could not find the host .uproject to launch")
            return 1
        uproject = dest_uproject or args.uproject
        ep.launch_editor(args.editor, uproject)
        print(f"restart: launched {args.editor.name} on {uproject.name}")
        return 0 if wait_for_handshake(args.wait, head) else 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
