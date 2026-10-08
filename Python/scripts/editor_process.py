#!/usr/bin/env python3
"""Editor lifecycle for the Unreal MCP agent: detect, reap, clean, restart.

A crashed or wedged editor does not go away on its own. If the next attempt just
launches another one, the result is a pile of orphaned windows and a port that a
zombie process may still own. This script is the single, project-scoped answer:

    status    what editors exist, which are ours, is the bridge answering, any crashes
    reap      force-kill our editors (or all editors with --all) so nothing is orphaned
    clean     remove the crash-recovery marker that raises the "restore unsaved files"
              modal on the next launch, and list/prune crash reports
    restart   reap -> clean -> launch exactly one editor -> wait for the bridge to answer

Reaping is scoped by the project's .uproject appearing in the editor command line, so
it never kills an editor someone opened on a different project.

The OS-touching functions are thin; the parsing/filtering helpers are pure and
unit-tested in test_editor_process.py.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import socket
import subprocess
import sys
import time

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[1]
DEFAULT_UPROJECT = REPO_ROOT / "MCPGameProject" / "MCPGameProject.uproject"
DEFAULT_EDITOR = pathlib.Path(
    r"C:\Program Files\Epic Games\UE_5.6\Engine\Binaries\Win64\UnrealEditor.exe"
)
SERVER_PY = REPO_ROOT / "Python" / "unreal_mcp_server.py"
HOST, PORT = "127.0.0.1", 55557

# ---------------------------------------------------------------------------
# Pure helpers (unit-tested without a live editor)
# ---------------------------------------------------------------------------


def parse_processes(raw: str) -> list[dict]:
    """Parse `Get-CimInstance ... | ConvertTo-Json` output into a list of dicts.

    PowerShell renders a single result as an object and several as an array, and
    emits nothing at all when there is no match; all three are handled.
    """
    text = (raw or "").strip()
    if not text:
        return []
    try:
        data = json.loads(text)
    except json.JSONDecodeError:
        return []
    items = data if isinstance(data, list) else [data]
    result = []
    for item in items:
        if not isinstance(item, dict):
            continue
        pid = item.get("ProcessId")
        if pid is None:
            continue
        result.append({
            "pid": int(pid),
            "created": str(item.get("CreationDate") or ""),
            "command_line": str(item.get("CommandLine") or ""),
        })
    return result


def _norm(path: str) -> str:
    return str(path).replace("\\", "/").rstrip("/").lower()


def format_cim_date(value: str) -> str:
    """Render a CIM `CreationDate` (`/Date(ms)/`) as local `YYYY-MM-DD HH:MM:SS`."""
    match = re.search(r"/Date\((\d+)\)/", value or "")
    if not match:
        return value or "unknown"
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(int(match.group(1)) / 1000.0))


def select_project_editors(processes: list[dict], uproject: pathlib.Path) -> list[int]:
    """PIDs of editors whose command line opens `uproject` (never a foreign project)."""
    needle = _norm(uproject)
    return [p["pid"] for p in processes if needle in _norm(p["command_line"])]


def newest_crash(dir_names_with_mtime: list[tuple[str, float]]) -> tuple[str, float] | None:
    """(name, mtime) of the most recently modified crash report, or None."""
    if not dir_names_with_mtime:
        return None
    return max(dir_names_with_mtime, key=lambda item: item[1])


def control_plane_secret(server_py: pathlib.Path) -> str:
    """Read the shared access key out of the server source (no import of the server)."""
    text = server_py.read_text(encoding="utf-8", errors="replace")
    match = re.search(r'CONTROL_PLANE_SECRET\s*=\s*"([0-9a-f]+)"', text)
    return match.group(1) if match else ""


# ---------------------------------------------------------------------------
# OS-touching helpers
# ---------------------------------------------------------------------------


def _powershell(command: str) -> str:
    try:
        completed = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", command],
            capture_output=True, text=True, timeout=30,
        )
        return completed.stdout or ""
    except Exception:
        return ""


def list_editors() -> list[dict]:
    raw = _powershell(
        "Get-CimInstance Win32_Process -Filter \"Name='UnrealEditor.exe'\" | "
        "Select-Object ProcessId,CreationDate,CommandLine | ConvertTo-Json -Compress"
    )
    return parse_processes(raw)


def probe_bridge(timeout: float = 3.0) -> str:
    """'ok' | 'wedged' | 'down'. A live-but-silent port is 'wedged', not 'down'."""
    secret = control_plane_secret(SERVER_PY)
    payload = json.dumps({"type": "ping", "params": {}, "access_key": secret})
    try:
        with socket.create_connection((HOST, PORT), timeout=timeout) as sock:
            sock.settimeout(timeout)
            sock.sendall(payload.encode("utf-8"))
            chunks = []
            while True:
                chunk = sock.recv(65536)
                if not chunk:
                    break
                chunks.append(chunk)
                try:
                    json.loads(b"".join(chunks).decode("utf-8"))
                    return "ok"
                except json.JSONDecodeError:
                    continue
    except (ConnectionRefusedError, socket.timeout, OSError):
        # Distinguish refused (nothing listening) from a socket that accepted but stalled.
        try:
            with socket.create_connection((HOST, PORT), timeout=timeout):
                return "wedged"
        except OSError:
            return "down"
    return "wedged"


def crash_report_dirs(crashes_dir: pathlib.Path) -> list[dict]:
    if not crashes_dir.is_dir():
        return []
    reports = []
    for entry in crashes_dir.iterdir():
        if entry.is_dir():
            reports.append({"name": entry.name, "path": str(entry), "mtime": entry.stat().st_mtime})
    return sorted(reports, key=lambda item: item["mtime"], reverse=True)


def kill_editor(pid: int) -> bool:
    _powershell(f"taskkill /PID {int(pid)} /F | Out-Null")
    return True


def launch_editor(editor: pathlib.Path, uproject: pathlib.Path) -> subprocess.Popen:
    # Detached so the editor outlives this script (and the agent turn).
    creation = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
    return subprocess.Popen(
        [str(editor), str(uproject), "-log", "-NoSplash", "-Unattended"],
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        creationflags=creation, close_fds=True,
    )


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------


def cmd_status(args) -> int:
    editors = list_editors()
    ours = set(select_project_editors(editors, args.uproject))
    reports = crash_report_dirs(args.uproject.parent / "Saved" / "Crashes")
    restore = (args.uproject.parent / "Saved" / "Autosaves" / "PackageRestoreData.json").exists()
    bridge = probe_bridge()

    if args.json:
        print(json.dumps({
            "editors": [dict(e, ours=e["pid"] in ours) for e in editors],
            "ours": sorted(ours),
            "bridge": bridge,
            "crash_reports": reports,
            "restore_data_present": restore,
        }, indent=2))
        return 0

    print(f"bridge: {bridge}  ({HOST}:{PORT})")
    if not editors:
        print("editors: none")
    for e in editors:
        tag = "ours" if e["pid"] in ours else "foreign"
        print(f"editor: pid={e['pid']} {tag} started={format_cim_date(e['created'])}")
    newest = newest_crash([(r["name"], r["mtime"]) for r in reports])
    print(f"crash reports: {len(reports)}" + (f" (newest: {newest[0]})" if newest else ""))
    print(f"restore marker present: {restore}")
    return 0


def cmd_reap(args) -> int:
    editors = list_editors()
    targets = [e["pid"] for e in editors] if args.all else select_project_editors(editors, args.uproject)
    if not targets:
        print(f"reap: no {'editors' if args.all else 'project editors'} to kill")
        return 0
    for pid in targets:
        kill_editor(pid)
        print(f"reap: killed pid {pid}")
    # Confirm they are actually gone.
    time.sleep(1.5)
    still = {e["pid"] for e in list_editors()} & set(targets)
    if still:
        print(f"reap: WARNING still alive: {sorted(still)}")
        return 1
    print(f"reap: {len(targets)} editor(s) gone")
    return 0


def cmd_clean(args) -> int:
    restore = args.uproject.parent / "Saved" / "Autosaves" / "PackageRestoreData.json"
    if restore.exists():
        restore.unlink()
        print(f"clean: removed {restore.name} (prevents the restore-unsaved-files modal)")
    else:
        print("clean: no restore marker")
    crashes = args.uproject.parent / "Saved" / "Crashes"
    reports = crash_report_dirs(crashes)
    print(f"clean: {len(reports)} crash report(s) under {crashes}")
    if args.prune_crashes and reports:
        import shutil
        for report in reports:
            shutil.rmtree(report["path"], ignore_errors=True)
        print(f"clean: pruned {len(reports)} crash report(s)")
    return 0


def cmd_restart(args) -> int:
    if not args.editor.exists():
        print(f"restart: editor binary not found: {args.editor}")
        return 1
    cmd_reap(argparse.Namespace(uproject=args.uproject, all=False))
    cmd_clean(argparse.Namespace(uproject=args.uproject, prune_crashes=False))
    launch_editor(args.editor, args.uproject)
    print(f"restart: launched {args.editor.name} (waiting up to {args.wait}s for the bridge)")
    deadline = time.time() + args.wait
    while time.time() < deadline:
        if probe_bridge(timeout=2.0) == "ok":
            print("restart: bridge is answering (ping ok)")
            return 0
        time.sleep(2.0)
    print("restart: bridge did not answer in time (check Saved/Logs/MCPGameProject.log)")
    return 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--uproject", default=str(DEFAULT_UPROJECT), type=pathlib.Path)
    parser.add_argument("--editor", default=str(DEFAULT_EDITOR), type=pathlib.Path)
    sub = parser.add_subparsers(dest="command", required=True)

    p_status = sub.add_parser("status", help="report editors, bridge liveness and crash state")
    p_status.add_argument("--json", action="store_true")
    p_status.set_defaults(func=cmd_status)

    p_reap = sub.add_parser("reap", help="force-kill editors so none are orphaned")
    p_reap.add_argument("--all", action="store_true", help="kill every UnrealEditor, not just this project's")
    p_reap.set_defaults(func=cmd_reap)

    p_clean = sub.add_parser("clean", help="clear the restore marker and report crash dirs")
    p_clean.add_argument("--prune-crashes", action="store_true")
    p_clean.set_defaults(func=cmd_clean)

    p_restart = sub.add_parser("restart", help="reap, clean, launch one editor, wait for ping")
    p_restart.add_argument("--wait", type=int, default=120)
    p_restart.set_defaults(func=cmd_restart)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
