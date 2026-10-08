"""Ad-hoc client-side MCP tool call: call_tool.py <tool> <json-params>."""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import unreal_mcp_server
from tools.asset_tools import register_asset_tools
from tools.editor_tools import register_editor_tools


class Registry:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def register(function):
            self.tools[function.__name__] = function
            return function
        return register


registry = Registry()
register_asset_tools(registry)
register_editor_tools(registry)

name = sys.argv[1]
params = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
result = registry.tools[name](None, **params)
print(json.dumps(result, indent=2)[:6000])
