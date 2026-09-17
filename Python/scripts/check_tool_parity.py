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


# ---------------------------------------------------------------------------
# Parameter-level drift (best effort)
#
# Name parity alone misses the case where both layers agree on a command name but
# disagree on its parameters (e.g. the Python layer sends "gravity_enabled" that the
# C++ handler never reads). We extract, per command, the parameter names the Python
# tool layer passes and the ones the C++ handler references, and flag parameters that
# are sent but never read.
#
# Extraction is deliberately conservative: when a side cannot be parsed confidently the
# command is skipped, never reported, so the check fails only on a clear signal. Known
# parsing gaps live in PARAM_ALLOWLIST.
# ---------------------------------------------------------------------------

CPP_HANDLER_CALL_RE = re.compile(r'(\w+)::(Handle\w+)\s*\(')
CPP_HANDLER_DEF_RE = re.compile(r'(\w+)::(Handle\w+)\s*\([^;{]*\)\s*\{')
CPP_PARAM_RE = re.compile(
    r'(?:TryGet|Has|Get)(?:String|Number|Bool|Array|Object)Field\(\s*TEXT\("([a-z0-9_]+)"\)'
    r'|(?:TryGet|Get|Has|Find)Field\(\s*TEXT\("([a-z0-9_]+)"\)'
    r'|Get(?:Vector|Rotator|Vector2D|IntArray|FloatArray)FromJson\([^,]*,\s*TEXT\("([a-z0-9_]+)"\)'
)
PY_SEND_RE = re.compile(r'send_command\(\s*["\']([a-z0-9_]+)["\']\s*,\s*')

# command -> parameter names the Python tool layer advertises but the C++ handler
# currently ignores. These are a KNOWN-DRIFT BASELINE, not a claim they are safe: each
# is a real "advertised but does nothing" gap to fix (the parameter has no effect on the
# backend). Listing them keeps CI green so only NEW drift fails; the checker still prints
# them under INFO so they stay visible. Remove an entry the moment the param is wired up.
PARAM_ALLOWLIST: dict[str, set[str]] = {
    # Empty: every advertised parameter is now read by its C++ handler. Add an entry (with
    # a reason) only for a genuine, accepted gap so new drift still fails CI.
}


def _dict_literal_keys(text: str, brace_index: int) -> set[str]:
    """Top-level string keys of the {...} dict literal whose '{' is at brace_index."""
    keys: set[str] = set()
    depth = 0
    i = brace_index
    while i < len(text):
        ch = text[i]
        if ch in "{([":
            depth += 1
        elif ch in "})]":
            depth -= 1
            if depth == 0:
                break
        elif ch in "\"'" and depth == 1:
            quote = ch
            j = i + 1
            buf: list[str] = []
            while j < len(text) and text[j] != quote:
                if text[j] == "\\" and j + 1 < len(text):
                    buf.append(text[j + 1])
                    j += 2
                    continue
                buf.append(text[j])
                j += 1
            k = j + 1
            while k < len(text) and text[k].isspace():
                k += 1
            if k < len(text) and text[k] == ":":
                keys.add("".join(buf))
            i = j
        i += 1
    return keys


PY_DEF_RE = re.compile(r'^[ \t]*def\s+\w+', re.M)


def _enclosing_def_span(text: str, pos: int) -> tuple[int, int]:
    """(start, end) of the def containing pos (last def before, next after; best effort)."""
    last = None
    for m in PY_DEF_RE.finditer(text):
        if m.start() <= pos:
            last = m
        else:
            break
    if last is None:
        return 0, len(text)
    nxt = PY_DEF_RE.search(text, last.end())
    return last.start(), (nxt.start() if nxt else len(text))


def collect_python_params(root: pathlib.Path) -> dict[str, set[str]]:
    """{command: {parameter names the Python tool layer passes}} (best effort)."""
    result: dict[str, set[str]] = {}
    for path in sorted(root.glob(PY_TOOLS_GLOB)):
        text = path.read_text(encoding="utf-8", errors="replace")
        for m in PY_SEND_RE.finditer(text):
            cmd = m.group(1)
            i = m.end()
            while i < len(text) and text[i].isspace():
                i += 1
            keys: set[str] = set()
            if i < len(text) and text[i] == "{":
                keys = _dict_literal_keys(text, i)
            else:
                j = i
                while j < len(text) and (text[j].isalnum() or text[j] == "_"):
                    j += 1
                ident = text[i:j]
                if ident:
                    start, end = _enclosing_def_span(text, m.start())
                    scope = text[start:end]
                    assign = re.search(r'\b' + re.escape(ident) + r'\s*=\s*\{', scope)
                    if assign:
                        keys = _dict_literal_keys(scope, scope.index("{", assign.start()))
            if keys:
                result.setdefault(cmd, set()).update(keys)
    return result


def collect_cpp_handler_params(root: pathlib.Path) -> dict[str, set[str]]:
    """{"Class::HandleX": {parameter names it reads}} (best effort)."""
    handlers: dict[str, set[str]] = {}
    for path in sorted(root.glob(CPP_COMMANDS_GLOB)):
        text = path.read_text(encoding="utf-8", errors="replace")
        for m in CPP_HANDLER_DEF_RE.finditer(text):
            key = m.group(2)
            depth = 0
            i = m.end() - 1
            while i < len(text):
                if text[i] == "{":
                    depth += 1
                elif text[i] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            body = text[m.end() - 1:i + 1]
            reads: set[str] = set()
            for pm in CPP_PARAM_RE.finditer(body):
                reads.add(pm.group(1) or pm.group(2) or pm.group(3))
            handlers.setdefault(key, set()).update(reads)
    return handlers


CPP_HANDLER_RETURN_RE = re.compile(r'return\s+(Handle\w+)\s*\(\s*Params\s*\)')


def collect_command_handlers(root: pathlib.Path) -> dict[str, str]:
    """{command: "HandleX"} from each command class's own HandleCommand dispatch."""
    handlers: dict[str, str] = {}
    for path in sorted(root.glob(CPP_COMMANDS_GLOB)):
        text = path.read_text(encoding="utf-8", errors="replace")
        for m in CPP_CMD_RE.finditer(text):
            cmd = m.group(1)
            hm = CPP_HANDLER_RETURN_RE.search(text, m.end(), m.end() + 200)
            if hm:
                handlers.setdefault(cmd, hm.group(1))
    return handlers


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

    # Parameter-level drift (best effort): a command present on both sides whose
    # Python-sent parameters are not all read by the matching C++ handler.
    cpp_handlers = collect_command_handlers(root)
    cpp_handler_params = collect_cpp_handler_params(root)
    py_params = collect_python_params(root)
    drift: dict[str, list[str]] = {}
    for cmd, sent in sorted(py_params.items()):
        if cmd in missing:
            continue  # name-level failure already reported
        handler = cpp_handlers.get(cmd)
        if not handler:
            continue
        reads = cpp_handler_params.get(handler)
        if reads is None:
            continue  # handler body not located; stay conservative
        unread = sent - reads - PARAM_ALLOWLIST.get(cmd, set())
        if unread:
            drift[cmd] = sorted(unread)

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

    if drift:
        print(f"FAIL: {len(drift)} command(s) send a parameter the C++ handler never reads:")
        for cmd in sorted(drift):
            print(f"  - {cmd}: {', '.join(drift[cmd])}")
        print()

    known_drift = {cmd: sorted(params) for cmd, params in PARAM_ALLOWLIST.items() if params}
    if known_drift and not args.quiet:
        print(f"INFO: {sum(len(v) for v in known_drift.values())} parameter(s) are advertised "
              f"by the Python layer but ignored by the C++ handler (tracked baseline):")
        for cmd in sorted(known_drift):
            print(f"  - {cmd}: {', '.join(known_drift[cmd])}")
        print()

    if missing:
        print("Parity check FAILED: the Python tool layer advertises commands the "
              "C++ plugin does not implement.")
        return 1
    if drift:
        print("Parity check FAILED: parameter drift between the Python tool layer "
              "and the C++ handlers.")
        return 1

    print("Parity check PASSED: every Python command is handled by the C++ plugin "
          "and no sent parameter is unread.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
