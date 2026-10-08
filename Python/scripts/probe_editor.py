"""Ad-hoc probe: runs a snippet in the editor via the MCP bridge."""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import unreal_mcp_server
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
register_editor_tools(registry)

code = sys.argv[1] if len(sys.argv) > 1 else "print('probe-ok')"
result = registry.tools["execute_python"](None, code=code)
print(json.dumps(result, indent=2))
