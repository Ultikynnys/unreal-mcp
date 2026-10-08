#!/usr/bin/env python3
"""Generate Docs/Tools/REFERENCE.md from the live tool catalog.

The prose under Docs/Tools/ is hand-written. This file is not: it exists so the complete
list of callable tools and their exact parameter names is documented by construction and
cannot drift when the surface changes.

    uv run --project Python python Python/scripts/gen_tool_docs.py           # (re)write it
    uv run --project Python python Python/scripts/gen_tool_docs.py --check   # fail if stale

test_docs_parity.py calls the same renderer, so a stale reference fails the suite.
"""

from __future__ import annotations

import difflib
import pathlib
import sys

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[1]
sys.path.insert(0, str(REPO_ROOT / "Python"))

import tool_catalog  # noqa: E402  (path set up above)

TOOLS_DIR = REPO_ROOT / "Python" / "tools"
DOC_PATH = REPO_ROOT / "Docs" / "Tools" / "REFERENCE.md"


def _cell(text: str) -> str:
    """Make a value safe inside a markdown table cell."""
    return text.replace("|", r"\|").replace("\n", " ").strip()


def render_reference_doc(catalog: list[tool_catalog.ToolInfo]) -> str:
    lines = [
        "# Unreal MCP tool reference",
        "",
        "Generated from the registered tools by `Python/scripts/gen_tool_docs.py`. "
        "Do not edit by hand: change the tool and re-run the script "
        "(`Python/scripts/test_docs_parity.py` fails when this file is stale).",
        "",
        f"{len(catalog)} tools. Parameters are the exact names the JSON schema accepts; "
        "`Sends` is the bridge command the tool issues, useful when correlating with "
        "editor logs. Replies are `{success, result, message}`: on failure the reason is "
        "in `error`/`message` and the structured detail stays in `result`.",
        "",
    ]
    by_category: dict[str, list[tool_catalog.ToolInfo]] = {}
    for tool in catalog:
        by_category.setdefault(tool.category, []).append(tool)
    for category in sorted(by_category):
        lines.append(f"## {category}")
        lines.append("")
        lines.append("| Tool | Parameters | Sends | Description |")
        lines.append("| --- | --- | --- | --- |")
        for tool in sorted(by_category[category], key=lambda t: t.name):
            lines.append(
                f"| `{tool.name}` | {_cell(', '.join(tool.params)) or '(none)'} "
                f"| `{_cell(tool.command)}` | {_cell(tool.summary)} |"
            )
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def main(argv: list[str]) -> int:
    catalog = tool_catalog.discover_tools(TOOLS_DIR)
    rendered = render_reference_doc(catalog)

    if "--check" in argv:
        if not DOC_PATH.is_file():
            print(f"docs: MISSING {DOC_PATH}")
            return 1
        current = DOC_PATH.read_text(encoding="utf-8")
        if current == rendered:
            print(f"docs: {DOC_PATH.name} is up to date ({len(catalog)} tools)")
            return 0
        diff = list(difflib.unified_diff(
            current.splitlines(), rendered.splitlines(),
            fromfile="on disk", tofile="regenerated", lineterm=""))
        print(f"docs: {DOC_PATH.name} is STALE - run gen_tool_docs.py")
        print("\n".join(diff[:40]))
        return 1

    DOC_PATH.parent.mkdir(parents=True, exist_ok=True)
    DOC_PATH.write_text(rendered, encoding="utf-8")
    print(f"docs: wrote {DOC_PATH} ({len(catalog)} tools)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
