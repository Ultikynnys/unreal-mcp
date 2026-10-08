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



## Revision mismatch: rebuild, do not guess

There is no hand-incremented number. The plugin bakes in the commit it was **built** from and the
server compares it against the commit it **runs** from, so drift cannot be forgotten:

| Side | Where |
|---|---|
| Python server | `git rev-parse HEAD` at import (`UNREAL_MCP_REVISION` overrides it) |
| Plugin build | `MCP_REVISION` define from `UnrealMCP.Build.cs`, baked in at build time |
| Loaded plugin | stamped as `revision` (plus `built_dirty`) on every reply |

The server pings first and refuses to send the command when the two differ, so nothing is
executed. Three refusals, all fail-closed:

- a different commit: the reply names both revisions and the rebuild;
- no `revision` field at all: the plugin predates the handshake;
- `unknown`: the plugin was built where git was unavailable.

The failure looks like this:

```text
Unreal MCP revision mismatch: this checkout is dab8994..., the running plugin reports e17c0a1...
... rebuild it and restart the editor before calling again.
```

That means the editor is running a stale `UnrealEditor-UnrealMCP.dll`. Do not retry and do not
launch another editor: reap, rebuild, restart.

```bash
uv run --project Python python Python/scripts/editor_process.py reap
"C:\Program Files\Epic Games\UE_5.6\Engine\Build\BatchFiles\Build.bat" MCPGameProjectEditor Win64 Development MCPGameProject/MCPGameProject.uproject -WaitMutex -NoHotReloadFromIDE
uv run --project Python python Python/scripts/editor_process.py restart --wait 180
```

The reap is not optional: a running editor holds `UnrealEditor-UnrealMCP.dll`, so the link step
fails with `LNK1104: cannot open file ... being used by another process`.

Two consequences worth knowing. Committing moves the server's revision immediately, so the
rebuild is forced on the next call until the plugin matches - that is the intent. And a plugin
**copied into another project** records that project's HEAD, which cannot match this repo: build
the copy with `MCP_REVISION=<this repo's HEAD>`, or set `UNREAL_MCP_REVISION` on the server. A
plugin built from a dirty tree is reported (`built_dirty`) and logged as a warning rather than
refused, because a commit cannot identify a dirty build and refusing would block the normal
edit-build-test loop.

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
  "modal_class": "SMessageDialog", "game_thread_stalled_seconds": 41.2
}
```

`state: busy` plus a climbing `game_thread_stalled_seconds` names the culprit: the command in
flight and the last modal on screen (`modal_title`, and its widget class in `modal_class`, which
is the useful half when the title is empty). `status` prints this as `bridge state: ...`, appends it to
a `wedged` verdict, and `--json` carries it as `snapshot`. Read the file directly when the
bridge socket itself is unreachable. A stale file is harmless: nothing is driven from it.

## What blocks a call: a stalled thread, not a window

Dispatch runs **on the game thread**, so a command can only go wrong by queueing behind a thread
that never ticks. The pre-flight therefore refuses on the **game-thread heartbeat**, not on the
presence of a window: past 15 s without a tick the call is refused before it is sent and nothing
is executed.

- `EDITOR_MODAL_ACTIVE` - a modal is holding the thread. The reply names the window's widget
  class and title and points at `recover_editor`.
- `EDITOR_BLOCKED` - the thread is stalled with no modal identified: a long operation is running.
  Retry shortly.
- A **progress window or a passive dialog does not block calls**. It leaves the thread ticking, so
  a long `move_assets` stays pollable through `get_job_status` for its whole run. Every refusal
  carries a `state` object (`game_thread_stalled_seconds`, `stalled_threshold_seconds`,
  `modal_class`, `modal_title`, `in_flight_command`) so the caller is not blind.
- Dispatch runs under `GIsSilent` (no slow-task progress window) and
  `GIsRunningUnattendedScript` (an engine prompt answers its default instead of opening a modal).

`ping`, `reload_server` and `recover_editor` stay reachable while a modal is up. `recover_editor`
is the way out.

## Long jobs are Tasks-shaped

A long operation (a move batch, a plan, an import) returns a `job_id` immediately. Poll
`get_job_status` / `get_plan_status` / `get_import_status`; the reply carries `phase`, `done` and
`total`. `queued`/`running` map to the MCP Tasks extension's `working`, `done` to `completed` and
`failed` to `failed`, so a client that speaks Tasks can treat a job id as a task handle. Push-based
`notifications/progress` is not implemented: the bridge answers one request per socket round-trip,
so progress is polled rather than streamed.

## A move batch ends by re-saving stale referencers

When a batch moves a referencer together with the package it depends on, the referencer's own save
can run while its dependency is still at its old path, and the later in-memory rewire does not
re-save it - the file on disk keeps the old import. Both move completion paths therefore end with a
sweep: for every moved source, ask the AssetRegistry for remaining referencers and resave them.
The job's `items` reports how many were resaved. This was found live (a material instance kept its
old parent path after a 323-asset batch). The disk, not the redirector count, is what shows it, so
verify a large move by scanning the moved packages for the old path strings.

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
