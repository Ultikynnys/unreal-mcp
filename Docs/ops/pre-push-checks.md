# Pre-push checks

One battery, two gates. `Python/scripts/run_checks.py` runs every offline check in one pass;
the tracked hook `.githooks/pre-push` runs it on `git push`, and
`.github/workflows/tool-parity.yml` runs the same command in CI. Because both call the same
runner, a local push and CI cannot drift apart.

Offline by design: no check needs a running editor, so a push never depends on one. The live
tool smoke is opt-in (`--live`) and is deliberately not part of the hook.

## Install (fresh clone)

```sh
git config core.hooksPath .githooks
```

Hooks live in `.githooks/` rather than `.git/hooks/` because only the former is versioned. The
hook is stored executable (`100755`) and `.gitattributes` pins its line endings to LF: a CRLF
shebang line makes sh refuse to run it, which would silently disable the gate on a Windows
checkout.

## Run it by hand

```sh
uv run --project Python python Python/scripts/run_checks.py           # the hook's battery
uv run --project Python python Python/scripts/run_checks.py --live     # also smoke the editor
uv run --project Python python Python/scripts/run_checks.py --quiet    # failures + summary only
```

Exit code is non-zero when any check fails. A check that cannot run (missing script) exits 127
and a wedged one is killed at 120s as exit 124; both fail closed rather than passing quietly.

## Bypass

`git push --no-verify` skips the hook. Use it knowingly: CI still runs the battery, so a bypass
turns a local failure into a red build instead of a blocked push.

## CI

`.github/workflows/tool-parity.yml` runs the same battery command, so the two gates cannot drift
apart. It installs the locked dependencies first (`uv sync --project Python --locked`), because
two checks import the tool modules and those import `mcp.server.fastmcp`: on a bare interpreter
they fail with `ModuleNotFoundError`, which is exactly how CI broke the first time the battery
was wired in. If any check fails to import, the runner prints the command that fixes it.

## What each check prevents

| Check | Prevents |
| --- | --- |
| parity checker unit test | A checker that silently stops detecting gaps |
| Python <-> C++ parity | A tool sending a command no handler serves; a routed-but-undispatched command; a parameter sent but never read; an **orphaned** command (served but no tool can reach it, the `focus_viewport` case); `get_capabilities` advertising a command that cannot be served or omitting one that can |
| agent-facing docs | A tool or parameter named in the instructions that does not exist; a docstring documenting a parameter that does not exist (the `suggested_placement` shape); an instruction naming a deleted file (`archive_mcp_client.py`); a tool with parameters that the smoke script never exercises; a hook that has been unwired or silenced |
| tool surface typing | A tool parameter typed `Any` / `object` / `Optional` / `Union`, which the project forbids |
| docs <-> tools parity | A documentation page citing a tool that does not exist, or missing a registered one |
| protocol version agreement | The Python declaration, the C++ macro and the plugin's reported version disagreeing |
| modal + failure guards | Losing the unattended-script guard, the modal pre-flight or the failure-reason passthrough (the dialog that could block an automated job) |
| editor lifecycle helpers | The reaper's project scoping, the snapshot-freshness rule, or the crash-marker cleanup regressing |
| asset tool wrappers | Move/preview defaults, the ambiguous-input guard, or the redirector-cleanup ordering in the C++ moving |
| C++ comment length | A comment block in the plugin growing past 3 lines: long comments are re-read on every view and every diff, and a comment that restates the code goes stale. One line preferred; raise or lower with `--max-lines` |
| workflow pins | A floating `runs-on: *-latest` label or an action pinned to a moving branch: both change CI without a commit here (ubuntu-latest moved 24.04 to 26 on 2026-10-19) |
| CI pin freshness | A pinned runner image, action or tool version passing its review date in `.github/pins.json`, or the ledger and the workflows disagreeing (a pin that is unregistered, or registered but gone) |
| battery runner | The runner treating a non-zero check as success (it must fail closed) |

## Pin shelf life

A pin is a promise with a date. GitHub retires runner images, deprecates action runtimes and
releases new action majors on its own schedule, none of which arrives as a commit here, so a
pinned CI value eventually rots. `.github/pins.json` records each pin with the date it must be
looked at again, and `check_pin_freshness.py` enforces it:

- an overdue `review_by` fails (pre-push and CI) and names how many days late it is;
- a pin due within 14 days prints a note without blocking;
- the ledger must match the workflows in **both** directions: a workflow pin that is not
  registered fails, and a registered pin that has vanished fails, so the ledger cannot rot
  silently either.

The workflow also runs weekly (`schedule`), so an aging pin is still raised in a quiet repository
rather than waiting for the next push. To clear a failure: follow the entry's `source`, update the
pinned value if needed, then set the next `review_by`.

## Adding a check

Add it to `CHECKS` in `Python/scripts/run_checks.py` and to the table above. Keep it offline: if
it needs the editor, put it in `LIVE_CHECKS` so the hook does not depend on a running instance.
