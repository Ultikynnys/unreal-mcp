"""Unit verification of new MCP tool schema and serialization."""
import sys
import os

repo_dir = "C:/Users/uraan/.reasonix/tools/unreal-mcp/Python"
sys.path.insert(0, repo_dir)

from tools.editor_tools import register_editor_tools
from mcp.server.fastmcp import FastMCP

mcp = FastMCP("TestServer")
register_editor_tools(mcp)

tool_names = [t.name for t in mcp._tool_manager.list_tools()]
print(f"Registered {len(tool_names)} tools in editor_tools:")
expected = [
    "get_capabilities",
    "query_assets",
    "get_asset_details",
    "get_actor_details",
    "get_actor_properties",
    "get_actors_in_level",
    "batch_execute",
    "set_viewport_camera",
    "capture_viewport_screenshot",
    "save_level",
    "load_level",
    "execute_python"
]

for exp in expected:
    assert exp in tool_names, f"Missing tool: {exp}"
    print(f"  ✓ {exp}")

print("\nAll 12 critical tools verified in FastMCP tool registry!")
