#!/usr/bin/env python3
"""CI parity check between the Python tool layer and the C++ UnrealMCP plugin.

Contract enforced: **Python subset of C++**.

Every command the Python MCP tool layer sends over the socket
(``unreal.send_command("...")`` in ``Python/tools/*.py``) must have a matching
handler in the C++ plugin's command dispatch. The Python layer must never expose
an MCP tool the plugin cannot serve.

Extra C++-internal commands (e.g. ``ping``) with no Python caller are allowed and
only reported for information.

Exits non-zero (1) when one or more Python commands have no C++ handler, so CI fails
on drift. Exits 0 when the two layers are in sync.

Run ``Python/scripts/test_check_tool_parity.py`` to confirm this check actually
detects a missing handler (negative test) and passes when in sync.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

# Repo root: this file lives at <repo>/Python/scripts/check_tool_parity.py
DEFAULT_ROOT = pathlib.Path(__file__).resolve().parents[2]

# C++ command routing lives in the bridge's ExecuteCommand(); individual handler
# classes add a few aliases (e.g. create_actor), so we union both sources.
CPP_BRIDGE_REL = "MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/UnrealMCPBridge.cpp"
CPP_COMMANDS_GLOB = "MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/Private/Commands/*.cpp"
PY_TOOLS_GLOB = "Python/tools/*.py"

# ``CommandType == TEXT("...")`` / ``CommandName == TEXT("...")`` in the C++ dispatch.
CPP_CMD_RE = re.compile(r'(?:CommandType|CommandName)\s*==\s*TEXT\("([a-z0-9_]+)"\)')
# ``unreal.send_command("<name>"`` in the Python tool layer.
PY_CMD_RE = re.compile(r'send_command\(\s*["\']([a-z0-9_]+)["\']')


def collect_cpp_commands(root: pathlib.Path) -> tuple[set[str], list[str]]:
    """Return (commands, files_scanned) handled by the C++ plugin."""
    files: list[pathlib.Path] = []
    bridge = root / CPP_BRIDGE_REL
    if bridge.exists():
        files.append(bridge)
    else:
        print(f"WARNING: bridge source not found: {bridge}", file=sys.stderr)
    files.extend(sorted(root.glob(CPP_COMMANDS_GLOB)))

    commands: set[str] = set()
    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        commands.update(CPP_CMD_RE.findall(text))
    return commands, [str(p.relative_to(root)) for p in files]


def collect_python_commands(root: pathlib.Path) -> tuple[dict[str, set[str]], list[str]]:
    """Return ({command: {tool files}}, files_scanned) sent by the Python layer."""
    commands: dict[str, set[str]] = {}
    files = sorted(root.glob(PY_TOOLS_GLOB))
    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        for command in PY_CMD_RE.findall(text):
            commands.setdefault(command, set()).add(path.name)
    return commands, [str(p.relative_to(root)) for p in files]


def analyze(root: pathlib.Path):
    """Return (cpp_commands, py_commands, missing, extra)."""
    cpp_commands, _ = collect_cpp_commands(root)
    py_commands, _ = collect_python_commands(root)
    missing = sorted(cmd for cmd in py_commands if cmd not in cpp_commands)  # Python - C++
    extra = sorted(cpp for cpp in cpp_commands if cpp not in py_commands)  # C++ - Python
    return cpp_commands, py_commands, missing, extra


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=str(DEFAULT_ROOT),
                        help="Repository root to scan (defaults to the checker's repo).")
    parser.add_argument("--quiet", action="store_true", help="only print on failure")
    args = parser.parse_args(argv)

    root = pathlib.Path(args.root).resolve()
    cpp_commands, cpp_files = collect_cpp_commands(root)
    py_commands, py_files = collect_python_commands(root)
    missing = sorted(cmd for cmd in py_commands if cmd not in cpp_commands)
    extra = sorted(cpp for cpp in cpp_commands if cpp not in py_commands)

    if not args.quiet:
        print(f"Scanned {len(cpp_files)} C++ file(s): {len(cpp_commands)} handled command(s)")
        print(f"Scanned {len(py_files)} Python tool file(s): {len(py_commands)} sent command(s)")
        print()

    if missing:
        print(f"FAIL: {len(missing)} Python command(s) have no C++ handler "
              f"(the plugin will return 'Unknown command'):")
        for cmd in missing:
            sources = ", ".join(sorted(py_commands[cmd]))
            print(f"  - {cmd}  (sent by {sources})")
        print()
    elif extra and not args.quiet:
        print(f"INFO: {len(extra)} C++ command(s) have no Python caller "
              f"(allowed; internal/alias commands):")
        for cmd in extra:
            print(f"  - {cmd}")
        print()

    if missing:
        print("Parity check FAILED: the Python tool layer advertises commands the "
              "C++ plugin does not implement.")
        return 1

    print("Parity check PASSED: every Python command is handled by the C++ plugin.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
