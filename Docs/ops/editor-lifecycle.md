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
uv run --project Python python Python/scripts/editor_process.py clean
uv run --project Python python Python/scripts/editor_process.py restart --wait 180
```

| Command | What it does |
|---|---|
| `status` | Lists every `UnrealEditor.exe` (pid, start time, command line) tagged `ours`/`foreign`; probes `127.0.0.1:55557` and reports `ok`/`wedged`/`down`; summarises `Saved/Crashes`; reports the restore marker. `--json` for machine use. |
| `reap` | Force-kills this project's editors (all editors with `--all`) and confirms they are gone. |
| `clean` | Removes `Saved/Autosaves/PackageRestoreData.json` (the file that raises the "restore unsaved files" modal on the next launch) and lists crash reports; `--prune-crashes` deletes them. |
| `restart` | `reap` -> `clean` -> launch one unattended editor -> poll until the bridge answers `ping`. |

## Reading `status`

- `bridge: down`, no editors -> nothing is running; go to `restart`.
- `bridge: wedged` -> a process is alive but not answering; it must be reaped.
- `bridge: ok` with one `ours` editor -> healthy; do not launch another.

`ok` / `wedged` / `down` distinguishes "dead" from "busy": a busy editor still answers
`ping`, a crashed one has no process and no listener.

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
