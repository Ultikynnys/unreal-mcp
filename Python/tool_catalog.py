"""Single source of truth for the agent-facing description of the MCP tool surface.

Discovery is deliberately decorator-based (`@mcp.tool()`). Looser rules (for example "any
function whose body calls send_command") also match commented-out tools and the register_*
wrappers, which inflates the count and puts phantom tools into the docs. Requiring the
decorator yields exactly what FastMCP actually registers.

Everything the agent reads about the tool surface (the server `instructions`, the `info`
prompt, and the coverage tests) is rendered from this one catalog, so it cannot drift.
"""

from __future__ import annotations

import ast
import pathlib

# Human-facing grouping, keyed by source module stem.
CATEGORY_LABELS = {
    "editor_tools": "Editor: actors, levels, viewport, captures, jobs, diagnostics",
    "blueprint_tools": "Blueprint assets: create, components, properties, compile, spawn",
    "node_tools": "Blueprint graph: nodes, pins, wiring, layout, validation",
    "umg_tools": "UMG widgets: create widget blueprints and their components",
    "project_tools": "Project settings: input mappings",
    "asset_tools": "Assets: query, move/rename, delete, redirectors, import, graphs",
}


class ToolInfo:
    def __init__(self, name: str, summary: str, params: list[str], command: str, category: str):
        self.name = name
        self.summary = summary
        self.params = params
        self.command = command
        self.category = category

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return f"ToolInfo({self.name!r}, params={self.params})"


def has_tool_decorator(node: ast.FunctionDef) -> bool:
    """True when the function carries `@mcp.tool()` - the definition of a registered tool."""
    for decorator in node.decorator_list:
        func = decorator.func if isinstance(decorator, ast.Call) else decorator
        if getattr(func, "attr", None) == "tool" or getattr(func, "id", None) == "tool":
            return True
    return False


def iter_decorated_functions(tools_dir: pathlib.Path):
    """Yield (module_path, FunctionDef) for every registered tool.

    Accepts either a directory of tool modules or a single module file.
    """
    paths = sorted(tools_dir.glob("*.py")) if tools_dir.is_dir() else [tools_dir]
    for module_path in paths:
        tree = ast.parse(module_path.read_text(encoding="utf-8"))
        for node in ast.walk(tree):
            if isinstance(node, ast.FunctionDef) and has_tool_decorator(node):
                yield module_path, node


def _command_of(node: ast.FunctionDef) -> str:
    """The send_command argument the tool uses, if any."""
    for child in ast.walk(node):
        if (isinstance(child, ast.Call)
                and isinstance(child.func, ast.Attribute)
                and child.func.attr == "send_command"
                and child.args
                and isinstance(child.args[0], ast.Constant)):
            return str(child.args[0].value)
    return ""


def _summary_of(node: ast.AST) -> str:
    doc = ast.get_docstring(node) or ""
    for line in doc.splitlines():
        line = line.strip()
        if line:
            return line
    return ""


def _params_of(node: ast.FunctionDef) -> list[str]:
    args = list(node.args.posonlyargs) + list(node.args.args) + list(node.args.kwonlyargs)
    return [a.arg for a in args if a.arg not in ("ctx", "self")]


def discover_tools(tools_dir: pathlib.Path) -> list[ToolInfo]:
    """Every registered MCP tool, in stable order, derived from the source."""
    tools: list[ToolInfo] = []
    for module_path, node in iter_decorated_functions(tools_dir):
        category = CATEGORY_LABELS.get(module_path.stem, module_path.stem)
        tools.append(ToolInfo(
            name=node.name,
            summary=_summary_of(node),
            params=_params_of(node),
            command=_command_of(node),
            category=category,
        ))
    tools.sort(key=lambda t: t.name)
    return tools


def render_tool_reference(catalog: list[ToolInfo]) -> str:
    """The full per-tool reference, grouped by category (used by the `info` prompt)."""
    lines = [
        f"# UnrealMCP tool reference ({len(catalog)} tools)",
        "",
        "Every tool is called with the parameters listed; the JSON schema is authoritative "
        "for types. Replies are `{success, result, message}`; on failure the reason is in "
        "`error`/`message` and structured detail stays in `result`.",
        "",
    ]
    by_category: dict[str, list[ToolInfo]] = {}
    for tool in catalog:
        by_category.setdefault(tool.category, []).append(tool)
    for category in sorted(by_category):
        lines.append(f"## {category}")
        for tool in by_category[category]:
            params = ", ".join(tool.params)
            summary = tool.summary or "(no description)"
            lines.append(f"- `{tool.name}({params})` - {summary}")
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def render_instructions(catalog: list[ToolInfo]) -> str:
    """Short orientation for the always-provided `instructions` field.

    Kept compact on purpose: this string is injected by the client, while the full per-tool
    detail lives in the on-demand `info` prompt.
    """
    groups = sorted({tool.category for tool in catalog})
    group_lines = "\n".join(f"- {group}" for group in groups)
    return (
        f"UnrealMCP drives a live Unreal Editor through a plugin bridge "
        f"({len(catalog)} tools). Use these tools only: do not open a raw socket to "
        f"127.0.0.1:55557, and do not re-implement a client.\n"
        f"\n"
        f"Surface:\n{group_lines}\n"
        f"\n"
        f"Call the `info` prompt for the full per-tool reference with parameter names.\n"
        f"\n"
        f"Replies are {{success, result, message}}. On failure the reason is in "
        f"`error`/`message` and structured detail (failed paths, per-item results) stays in "
        f"`result`. Machine-readable codes: EDITOR_MODAL_ACTIVE means a modal is on screen "
        f"(call recover_editor, then retry); EDITOR_BUSY means the editor is saving or "
        f"garbage-collecting (retry shortly).\n"
        f"\n"
        f"Long operations return a job_id: poll get_job_status, get_import_status or "
        f"get_plan_status rather than assuming completion.\n"
        f"\n"
        f"If a call cannot connect or times out, the editor may be gone or wedged. Do NOT "
        f"launch a second editor: run `uv run --project Python python "
        f"Python/scripts/editor_process.py status`, then `reap` and `restart`. "
        f"MCPGameProject/Saved/MCP/bridge_state.json carries an out-of-band snapshot (state, "
        f"current command, game_thread_stalled_seconds, modal_title) that answers even when "
        f"the game thread is blocked.\n"
        f"\n"
        f"If a call fails with 'Unreal plugin version mismatch' or 'protocol drift', the "
        f"plugin and this server disagree (a stale plugin build, or a checkout from a "
        f"different revision). The failure names the fix: rebuild the plugin and/or update "
        f"the checkout as it says, then restart the editor. Every call fails until the three "
        f"agree, so do not retry."
    )
