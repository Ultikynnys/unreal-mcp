#!/usr/bin/env python3
"""Run every offline check in one pass; exit non-zero when any of them fails.

This is the single battery behind both the pre-push hook (.githooks/pre-push) and CI
(.github/workflows/tool-parity.yml), so a local push and CI enforce exactly the same set and
cannot drift apart.

Offline by design: nothing here needs a running editor, because a push must not depend on one.
The live smoke is opt-in via --live and is never part of the hook.

Each check runs in its own process. A checker that crashes, hangs or leaves a corrupted
interpreter behind cannot take the rest of the battery with it; a timeout is reported as a
failure rather than hanging the push.

    python Python/scripts/run_checks.py            # the hook's battery
    python Python/scripts/run_checks.py --live      # also smoke the running editor
    python Python/scripts/run_checks.py --quiet     # only print failures and the summary
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
import time

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[1]

# Order matters in one place: the parity checker's own unit test runs before the checker, so a
# broken checker surfaces as "the checker is broken" instead of a false failure of the tree.
CHECKS: list[tuple[str, str, tuple[str, ...]]] = [
    # The parity gate is first and unmissable: it is the contract that keeps the Python tool
    # layer and the C++ plugin in sync, and it must block a push on its own.
    ("parity checker unit test", "test_check_tool_parity.py", ()),
    ("Python <-> C++ parity", "check_tool_parity.py", ()),
    ("agent-facing docs", "test_agent_docs.py", ()),
    ("tool surface typing", "test_tool_surface.py", ()),
    ("docs <-> tools parity", "test_docs_parity.py", ()),
    ("protocol version agreement", "test_protocol_version.py", ()),
    ("modal + failure guards", "test_modal_and_failure_guards.py", ()),
    ("editor lifecycle helpers", "test_editor_process.py", ()),
    ("asset tool wrappers", "test_asset_tools.py", ()),
    ("battery runner", "test_run_checks.py", ()),
]

# Needs a live editor bridge, so it is never part of the pre-push hook.
LIVE_CHECKS: list[tuple[str, str, tuple[str, ...]]] = [
    ("live tool smoke", "smoke_all_tools.py", ("--timeout", "30")),
]

# Every check must finish in seconds; the whole battery is ~10s. A check that needs longer is
# a bug to fix, and a wedged one must not hold a push open for 10 minutes.
TIMEOUT_SECONDS = 120
OUTPUT_TAIL_LINES = 25


def summarize(results: list[tuple[str, int]]) -> tuple[int, str]:
    """(exit_code, one-line summary) for a list of (label, returncode)."""
    failed = [label for label, code in results if code != 0]
    summary = f"{len(results) - len(failed)}/{len(results)} checks passed"
    if failed:
        summary += "; failed: " + ", ".join(failed)
    return (1 if failed else 0), summary


def run_check(label: str, script: str, extra: tuple[str, ...] = (),
              timeout: int = TIMEOUT_SECONDS) -> tuple[str, int, float, str]:
    """Run one check in a subprocess. Returns (label, returncode, seconds, output_tail).

    A missing script is returncode 127, a timeout 124: both fail closed, and neither can be
    confused with success by a caller that only looks at non-zero.
    """
    path = SCRIPT_DIR / script
    if not path.exists():
        return label, 127, 0.0, f"missing check script: {path}"
    started = time.monotonic()
    try:
        completed = subprocess.run(
            [sys.executable, str(path), *extra],
            cwd=str(REPO_ROOT), capture_output=True, text=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return label, 124, time.monotonic() - started, f"timed out after {timeout}s"
    seconds = time.monotonic() - started
    output = (completed.stdout or "") + (completed.stderr or "")
    tail = "\n".join(output.strip().splitlines()[-OUTPUT_TAIL_LINES:])
    return label, completed.returncode, seconds, tail


def format_line(label: str, code: int, seconds: float) -> str:
    status = "ok  " if code == 0 else f"FAIL({code})"
    return f"  {status:<10} {label:<30} {seconds:5.2f}s"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--live", action="store_true",
                        help="also run the live tool smoke (requires a running editor)")
    parser.add_argument("--quiet", action="store_true", help="print failures and summary only")
    args = parser.parse_args(argv)

    checks = CHECKS + (LIVE_CHECKS if args.live else [])
    if not args.quiet:
        print(f"running {len(checks)} check(s) from {REPO_ROOT}", flush=True)
    results: list[tuple[str, int]] = []
    details: list[tuple[str, int, str]] = []
    for label, script, extra in checks:
        name, code, seconds, tail = run_check(label, script, extra)
        results.append((name, code))
        if code != 0:
            details.append((name, code, tail))
        if not args.quiet or code != 0:
            print(format_line(name, code, seconds), flush=True)

    for name, code, tail in details:
        print(f"\n--- {name} failed (exit {code}) ---")
        print(tail or "(no output)")

    code, summary = summarize(results)
    print(f"\n{summary}")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
