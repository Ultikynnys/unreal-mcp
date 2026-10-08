# Editor lifecycle: reap a crashed editor instead of stacking another

Unreal editors crash, and they wedge when a modal sits on the game thread. A crashed or
wedged editor does not clean itself up: if the next attempt just launches another one you
get orphaned windows and a port that a zombie process may still own. The rule is simple:
**reap, then launch exactly one.**

`Python/scripts/editor_process.py` is the tool for that. It is scoped to this project's
`.uproject` (matched in the editor command line), so it never kills an editor opened on a
different project.

## Commands

```bash
uv run --project Python python Python/scripts/editor_process.py status
uv run --project Python python Python/scripts/editor_process.py reap
uv run --project Python python Python/scripts/editor_process.py reap --orphans-only
uv run --project Python python Python/scripts/editor_process.py clean
uv run --project Python python Python/scripts/editor_process.py restart --wait 180
```

| Command | What it does |
|---|---|
| `status` | Lists every editor and editor-helper process (pid, start time, command line); editors tagged `ours`/`foreign`, helpers tagged `ok (owner <pid>)`/`ORPHAN (owner gone)`. Probes `127.0.0.1:55557` and reports `ok`/`wedged`/`down`; summarises `Saved/Crashes`; reports the restore marker and the orphan count. `--json` for machine use. |
| `reap` | Force-kills this project's editors plus any orphaned helpers (every editor with `--all`) and confirms they are gone. |
| `reap --orphans-only` | Kills only orphaned helpers, leaving a working editor and its helpers alone. |
| `clean` | Removes `Saved/Autosaves/PackageRestoreData.json` (the file that raises the "restore unsaved files" modal on the next launch) and lists crash reports; `--prune-crashes` deletes them. |
| `restart` | `reap` -> `clean` -> launch one unattended editor -> poll until the bridge answers `ping`. |

## Reading `status`

- `bridge: down`, no editors -> nothing is running; go to `restart`.
- `bridge: wedged` -> a process is alive but not answering; it must be reaped.
- `bridge: ok` with one `ours` editor -> healthy; do not launch another.

`ok` / `wedged` / `down` distinguishes "dead" from "busy": a busy editor still answers
`ping`, a crashed one has no process and no listener.

## The out-of-band state file

The bridge writes `MCPGameProject/Saved/MCP/bridge_state.json` from a thread that is **not**
the game thread, every 250 ms. Every command is dispatched on the game thread, so when the
game thread is blocked (a modal no one can answer, a long save, a slow task) nothing over the
socket answers, not even `ping`. This file keeps moving, so the reason survives.

```json
{
  "pid": 28956, "project": "MCPGameProject", "state": "busy",
  "command": "save_level", "command_elapsed_seconds": 41.0,
  "last_command": "get_capabilities", "last_command_seconds": 0.1,
  "last_error": "", "modal_title": "Redirector Update Report",
  "game_thread_stalled_seconds": 41.2
}
```

`state: busy` plus a climbing `game_thread_stalled_seconds` names the culprit: the command in
flight and the last modal on screen. `status` prints this as `bridge state: ...`, appends it to
a `wedged` verdict, and `--json` carries it as `snapshot`. Read the file directly when the
bridge socket itself is unreachable. A stale file is harmless: nothing is driven from it.

## Helper processes outlive a crash

An editor also spawns helpers: `CrashReportClientEditor.exe` (the crash-reporter window) and
`UnrealTraceServer.exe`. They carry no project path, so they are attributed by the pid they
monitor/sponsor (`-MONITOR=<pid>` or `--sponsor <pid>`). When that owner is gone the helper is
an **orphan** - a leftover window from a dead editor - and `status` reports it as
`ORPHAN (owner gone)`. `reap` (default) and `reap --orphans-only` both remove it; helpers whose
owner is alive are left untouched.

## Why not just retry

- A timed-out or `Failed to connect` tool call is usually the editor being gone, not a
  hiccup. Retrying blindly is what accumulates windows.
- `recover_editor` (the MCP tool) dismisses a modal that is stuck on screen and clears the
  restore marker; call it when the bridge still answers. If the process is wedged or
  crashed, only `reap` helps.
- A crash is visible in `MCPGameProject/Saved/Logs/MCPGameProject.log`: the run ends with
  `LogExit: Executing StaticShutdownAfterError` and a callstack naming the failing module.

## For the agent

A project skill (`editor-lifecycle`, `.reasonix/skills/`) encodes this same loop so future
agent turns follow it automatically. That skill directory is gitignored, so this document
is the versioned source of truth; keep the two in step.
