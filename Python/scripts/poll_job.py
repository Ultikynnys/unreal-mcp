"""Poll an UnrealMCP job until done/failed and print the terminal status."""
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import unreal_mcp_server
from tools.asset_tools import register_asset_tools


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

job_id = sys.argv[1]
deadline = time.monotonic() + float(sys.argv[2] if len(sys.argv) > 2 else 900)
last = None
while time.monotonic() < deadline:
    try:
        status = registry.tools["get_job_status"](None, job_id=job_id)
        result = status.get("result", status)
        state = result.get("state")
        if state != last:
            print(state, result.get("done"), "/", result.get("total"), result.get("phase", ""), flush=True)
            last = state
        if state in ("done", "failed"):
            print(json.dumps(result, indent=2)[:4000])
            sys.exit(0 if state == "done" else 1)
    except Exception as error:
        print("transport:", str(error)[:120], flush=True)
    time.sleep(2)
print("TIMEOUT", flush=True)
sys.exit(2)
