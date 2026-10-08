#!/usr/bin/env python3
"""Fail when a comment in the C++ plugin runs longer than N lines (default 3).

Long comments are token wastage: they are re-read on every file view and every diff, and a
comment that restates the code goes stale the moment the code changes. The project rule is 3
lines or fewer, one line preferred.

Only the C++ plugin is scanned (MCPGameProject/Plugins/UnrealMCP/Source). One state machine
walks the text so that a ``//`` inside a literal is not a comment, a quote inside a comment is
not a literal (comments say things like the "World memory leaks" ensure), and an unterminated
literal cannot swallow the rest of the file.

    python Python/scripts/check_comment_length.py                 # scan the plugin
    python Python/scripts/check_comment_length.py --max-lines 3   # the rule
    python Python/scripts/check_comment_length.py --root <dir>    # scan something else
"""

from __future__ import annotations

import argparse
import pathlib
import sys

DEFAULT_ROOT = pathlib.Path(__file__).resolve().parents[2]
# pathlib.glob has no brace expansion, so the two extensions are listed separately.
PLUGIN_SOURCE_PATTERNS = (
    "MCPGameProject/Plugins/UnrealMCP/Source/**/*.cpp",
    "MCPGameProject/Plugins/UnrealMCP/Source/**/*.h",
)
MAX_COMMENT_LINES = 3
PREVIEW_CHARS = 90


def comment_regions(text: str) -> list[list[int | str]]:
    """[[start_line, end_line, kind]] for every comment, 1-indexed.

    kind is 'line' (a comment-only line), 'trailing' (code then //), or 'block' (/*...*/).
    A //-comment with no closing newline ends at the end of the file.
    """
    regions: list[list[int | str]] = []
    line, i, n = 1, 0, len(text)
    state = "code"
    block_start = 1
    while i < n:
        char = text[i]

        if state == "code":
            if char == "/" and i + 1 < n and text[i + 1] == "/":
                # Only a comment-only line may join a block: seven trailing comments on seven
                # code lines are not one seven-line comment.
                line_start = text.rfind("\n", 0, i) + 1
                kind = "line" if text[line_start:i].strip() == "" else "trailing"
                regions.append([line, line, kind])
                state = "line_comment"
                i += 2
                continue
            if char == "/" and i + 1 < n and text[i + 1] == "*":
                block_start = line
                state = "block_comment"
                i += 2
                continue
            if char == '"':
                state = "string"
            elif char == "'":
                state = "char"
            elif char == "\n":
                line += 1
            i += 1
            continue

        if state == "line_comment":
            if char == "\n":
                line += 1
                state = "code"
            i += 1
            continue

        if state == "block_comment":
            if char == "*" and i + 1 < n and text[i + 1] == "/":
                regions.append([block_start, line, "block"])
                state = "code"
                i += 2
                continue
            if char == "\n":
                line += 1
            i += 1
            continue

        # string / char literal: a quote in here closes it, an escape is skipped, and a raw
        # newline ends it so an unterminated literal cannot blank the rest of the file.
        quote = '"' if state == "string" else "'"
        if char == "\\" and i + 1 < n:
            if text[i + 1] == "\n":
                line += 1
            i += 2
            continue
        if char == quote:
            state = "code"
            i += 1
            continue
        if char == "\n":
            line += 1
            state = "code"
        i += 1

    if state == "block_comment":
        regions.append([block_start, line, "block"])
    return regions


def merge_adjacent(regions: list[list[int | str]]) -> list[list[int | str]]:
    """Join touching comment-only regions into one block.

    Four consecutive //-lines are one 4-line comment, not four 1-line comments, so they are
    measured together. A blank line, a code line, or a trailing comment keeps them separate.
    """
    merged: list[list[int | str]] = []
    for start, end, kind in regions:
        joins = (kind == "line" and merged and merged[-1][2] in ("line", "mixed")
                 and start <= merged[-1][1] + 1)
        if joins:
            merged[-1][1] = max(merged[-1][1], end)  # type: ignore[arg-type]
            merged[-1][2] = "mixed"
        else:
            merged.append([start, end, kind])
    return merged


def long_comments(text: str, max_lines: int = MAX_COMMENT_LINES) -> list[tuple[int, int, int]]:
    """[(start_line, end_line, line_count)] for every comment longer than max_lines."""
    spans = []
    for start, end, _kind in merge_adjacent(comment_regions(text)):
        count = end - start + 1  # type: ignore[operator]
        if count > max_lines:
            spans.append((start, end, count))  # type: ignore[arg-type]
    return spans


def scan_file(path: pathlib.Path, max_lines: int = MAX_COMMENT_LINES) -> list[str]:
    """Human-readable findings for one file (empty when it is clean)."""
    text = path.read_text(encoding="utf-8", errors="replace")
    lines = text.split("\n")
    findings = []
    for start, end, count in long_comments(text, max_lines):
        preview = lines[start - 1].strip()[:PREVIEW_CHARS]
        findings.append(f"  {path}:{start}-{end} ({count} lines): {preview}")
    return findings


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=str(DEFAULT_ROOT))
    parser.add_argument("--max-lines", type=int, default=MAX_COMMENT_LINES)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)

    root = pathlib.Path(args.root).resolve()
    files = sorted({path for pattern in PLUGIN_SOURCE_PATTERNS for path in root.glob(pattern)})
    findings: list[str] = []
    for path in files:
        findings.extend(scan_file(path, args.max_lines))

    if not args.quiet:
        print(f"Scanned {len(files)} C++ source file(s) for comments longer than "
              f"{args.max_lines} lines")

    if findings:
        print(f"FAIL: {len(findings)} comment block(s) exceed {args.max_lines} lines "
              f"(token wastage; keep 3 or fewer, one preferred):")
        print("\n".join(findings))
        return 1

    if not args.quiet:
        print(f"OK: no comment block exceeds {args.max_lines} lines")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
