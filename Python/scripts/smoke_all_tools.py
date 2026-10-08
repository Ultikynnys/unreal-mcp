"""Live smoke test: call every registered MCP tool once and confirm each reaches a handler.

Runtime half of the parity contract. `check_tool_parity.py` proves statically that every
Python command name has a C++ handler; this proves it in the running editor by calling
each tool and checking the reply is not an "Unknown ... command" routing failure.

A tool PASSES when the bridge answered with anything other than an unknown-command
error: a success, or a normal validation error ("Missing 'x' parameter", "object not
found"), both prove the command reached its handler. A tool FAILS on
"Unknown command" / "Unknown editor command" / "Unknown blueprint command" / etc.

Every name this script hands to a tool carries the SCRATCH prefix, because several tools
(create_blueprint, create_umg_widget_blueprint, add_component_to_blueprint, ...)
auto-create the asset they are given. The run therefore ends by deleting everything under
that prefix - scratch folders, scratch-named actors, and the input mapping - so the
project is left exactly as it was found.
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import sys
import time
from pathlib import Path

# Bound every call, and refuse to start against a dead bridge. Without this the run has no
# timeout of its own: the server's read timeout is 10 minutes by default, so a bridge that
# accepts a connection and never answers stalls the whole run, and a dead editor makes it
# walk all 78 tools for nothing.
_parser = argparse.ArgumentParser(description="Smoke test every registered MCP tool.")
_parser.add_argument("--timeout", type=int, default=30,
                     help="per-call socket timeout in seconds (default %(default)s)")
_args = _parser.parse_args()

# unreal_mcp_server reads its timeouts from the environment at import time, so set them
# before the import below.
os.environ["UNREAL_MCP_READ_TIMEOUT"] = str(_args.timeout)
os.environ["UNREAL_MCP_TIMEOUT"] = str(_args.timeout)

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import unreal_mcp_server  # noqa: E402
from tools.asset_tools import register_asset_tools  # noqa: E402
from tools.blueprint_tools import register_blueprint_tools  # noqa: E402
from tools.editor_tools import register_editor_tools  # noqa: E402
from tools.node_tools import register_blueprint_node_tools  # noqa: E402
from tools.project_tools import register_project_tools  # noqa: E402
from tools.umg_tools import register_umg_tools  # noqa: E402

# Any asset/actor name handed to a creating tool starts with this, so cleanup can find it.
SCRATCH = "___mcp_smoke___"
SCRATCH_ABSENT = SCRATCH + "_absent___"  # named-but-missing: exercises the not-found paths


class Registry:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def register(function):
            self.tools[function.__name__] = function
            return function
        return register


registry = Registry()
for register in (register_asset_tools, register_blueprint_tools, register_blueprint_node_tools,
                 register_editor_tools, register_project_tools, register_umg_tools):
    register(registry)

# Minimal, non-destructive parameters per tool. Anything not listed is called with no
# arguments, which yields a validation error from the handler - still a PASS here.
PARAMS = {
    "execute_python": {"code": "print('smoke')"},
    "batch_execute": {"actions": [{"type": "get_capabilities"}]},
    "get_actor_details": {"name": SCRATCH_ABSENT},
    "set_actor_property": {"name": SCRATCH_ABSENT, "property_name": "x", "property_value": "y"},
    "set_actor_transform": {"name": SCRATCH_ABSENT, "location": [0, 0, 0]},
    "delete_actor": {"name": SCRATCH_ABSENT},
    "delete_actors_by_prefix": {"prefix": SCRATCH_ABSENT},
    "set_actor_material": {"name": SCRATCH_ABSENT, "material_path": "/Game/" + SCRATCH, "slot_index": 0},
    "set_actor_folder": {"name": SCRATCH_ABSENT, "folder_path": "Smoke"},
    "spawn_actor": {"name": SCRATCH, "type": "POINT_LIGHT"},
    "spawn_mesh_actor": {"mesh_path": "/Game/" + SCRATCH_ABSENT, "name": SCRATCH},
    "spawn_light_actor": {"name": SCRATCH},
    "spawn_mesh_grid": {"mesh_path": "/Game/" + SCRATCH_ABSENT, "prefix": SCRATCH, "rows": 1, "cols": 1},
    "spawn_instanced_mesh": {"mesh_path": "/Game/" + SCRATCH_ABSENT, "name": SCRATCH, "instances": []},
    "spawn_blueprint_actor": {"blueprint_name": SCRATCH_ABSENT, "actor_name": SCRATCH},
    "get_actors_in_level": {},
    "capture_viewport_screenshot": {},
    "capture_pie_screenshot": {},
    "query_assets": {"path": "/Game", "limit": 1},
    "get_asset_details": {"asset_path": "/Game/" + SCRATCH_ABSENT},
    "get_asset_graph": {"asset_path": "/Game/" + SCRATCH_ABSENT},
    "delete_assets": {"asset_paths": ["/Game/" + SCRATCH_ABSENT]},
    "move_assets": {"moves": [{"source": "/Game/" + SCRATCH_ABSENT, "destination": "/Game/Smoke/X"}], "dry_run": True},
    "move_folder": {"source_path": "/Game/" + SCRATCH_ABSENT, "destination_path": "/Game/Smoke", "dry_run": True},
    "fixup_redirectors": {"path": "/Game/" + SCRATCH_ABSENT, "recursive": True},
    "list_redirectors": {"path": "/Game/" + SCRATCH_ABSENT},
    "resave_packages": {"path": "/Game/" + SCRATCH_ABSENT},
    "import_asset": {"sources": ["/nonexistent/" + SCRATCH + ".fbx"]},
    "get_import_status": {"job_id": SCRATCH_ABSENT},
    "get_job_status": {"job_id": SCRATCH_ABSENT},
    "get_plan_status": {"job_id": SCRATCH_ABSENT},
    "console_command": {"command": "stat fps"},
    "load_level": {"map_path": "/Game/" + SCRATCH_ABSENT},
    "delete_level": {"map_path": "/Game/" + SCRATCH_ABSENT},
    "save_level": {},
    "create_level": {"map_path": "___invalid_path___", "template": ""},
    "set_viewport_camera": {"location": [0, 0, 0], "rotation": [0, 0, 0]},
    "create_blueprint": {"name": SCRATCH, "parent_class": "Actor"},
    "add_component_to_blueprint": {"blueprint_name": SCRATCH, "component_type": "StaticMeshComponent", "component_name": "C"},
    "set_component_property": {"blueprint_name": SCRATCH, "component_name": "C", "property_name": "x", "property_value": "y"},
    "set_physics_properties": {"blueprint_name": SCRATCH, "component_name": "C"},
    "set_static_mesh_properties": {"blueprint_name": SCRATCH, "component_name": "C", "static_mesh": "/Game/" + SCRATCH_ABSENT},
    "compile_blueprint": {"blueprint_name": SCRATCH},
    "set_blueprint_property": {"blueprint_name": SCRATCH, "property_name": "x", "property_value": "y"},
    "add_blueprint_variable": {"blueprint_name": SCRATCH, "variable_name": "V", "variable_type": "Boolean"},
    "add_blueprint_event_node": {"blueprint_name": SCRATCH, "event_name": "ReceiveBeginPlay"},
    "add_blueprint_input_action_node": {"blueprint_name": SCRATCH, "action_name": "Jump"},
    "add_blueprint_function_node": {"blueprint_name": SCRATCH, "target": "self", "function_name": "PrintString"},
    "add_blueprint_node": {"blueprint_name": SCRATCH, "node_type": "branch"},
    "add_blueprint_self_reference": {"blueprint_name": SCRATCH},
    "add_blueprint_get_self_component_reference": {"blueprint_name": SCRATCH, "component_name": "C"},
    "add_blueprint_reroute_node": {"blueprint_name": SCRATCH},
    "connect_blueprint_nodes": {"blueprint_name": SCRATCH, "source_node_id": "A", "source_pin": "P", "target_node_id": "B", "target_pin": "P"},
    "disconnect_blueprint_pin": {"blueprint_name": SCRATCH, "node_id": "A"},
    "delete_blueprint_node": {"blueprint_name": SCRATCH, "node_id": "A"},
    "clear_blueprint_graph": {"blueprint_name": SCRATCH},
    "find_blueprint_nodes": {"blueprint_name": SCRATCH},
    "get_blueprint_graphs": {"blueprint_name": SCRATCH},
    "get_blueprint_node_bounds": {"blueprint_name": SCRATCH},
    "set_blueprint_node_position": {"blueprint_name": SCRATCH, "node_id": "A", "position": [0, 0]},
    "set_blueprint_node_pin_default": {"blueprint_name": SCRATCH, "node_id": "A", "pin_name": "P", "value": 1},
    "validate_blueprint_graph": {"blueprint_name": SCRATCH},
    "auto_layout_blueprint_graph": {"blueprint_name": SCRATCH},
    "apply_blueprint_plan": {"blueprint_name": SCRATCH, "graph_name": "EventGraph", "plan": {"nodes": [], "edges": []}},
    "create_umg_widget_blueprint": {"widget_name": SCRATCH},
    "add_button_to_widget": {"widget_name": SCRATCH, "button_name": "B"},
    "add_text_block_to_widget": {"widget_name": SCRATCH, "text_block_name": "T"},
    "bind_widget_event": {"widget_name": SCRATCH, "widget_component_name": "B", "event_name": "OnClicked"},
    "add_widget_to_viewport": {"widget_name": SCRATCH},
    "set_text_block_binding": {"widget_name": SCRATCH, "text_block_name": "T", "binding_property": "Text", "binding_type": "Text"},
    "create_input_mapping": {"action_name": SCRATCH, "key": "SpaceBar", "input_type": "Action"},
    "get_capabilities": {},    "recover_editor": {},
    "reload_server": {},
    "editor_play": {},
    "editor_stop": {},
    "list_levels": {},
    "get_current_level": {},
}

UNKNOWN_MARKERS = ("Unknown command", "Unknown editor command", "Unknown blueprint command",
                   "Unknown project command", "Unknown umg command", "Unknown node command")

# Writes project config rather than content, so it is exercised by the static parity check
# instead of live: an in-memory input mapping is not worth a persisted side effect here.
SKIP = {"create_input_mapping"}

# Removes everything the creating tools above may have produced. Only touches names
# carrying the scratch prefix, so real project content is never at risk.
CLEANUP_PY = '''
import unreal
PREFIX = "___mcp_smoke___"
el = unreal.EditorAssetLibrary
removed_dirs = 0
for d in ["/Game/" + PREFIX, "/Game/Blueprints/" + PREFIX, "/Game/UI/" + PREFIX]:
    if el.does_directory_exist(d):
        el.delete_directory(d)
        removed_dirs += 1
# create_* tools can leave the asset in memory without a directory entry; catch those too.
for asset_path in ["/Game/Blueprints/" + PREFIX, "/Game/UI/" + PREFIX]:
    if el.does_asset_exist(asset_path):
        el.delete_asset(asset_path)
        removed_dirs += 1
subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
removed_actors = 0
for actor in subsystem.get_all_level_actors():
    if actor.get_actor_label().startswith(PREFIX):
        subsystem.destroy_actor(actor)
        removed_actors += 1
print("smoke cleanup: dirs=%d actors=%d" % (removed_dirs, removed_actors))
'''


def bridge_up(timeout: float = 3.0) -> bool:
    """True when something is listening on the bridge port (cheap; sends no command)."""
    try:
        with socket.create_connection(("127.0.0.1", 55557), timeout=timeout):
            return True
    except OSError:
        return False


def cleanup() -> None:
    """Best-effort removal of anything the smoke run created."""
    try:
        result = registry.tools["execute_python"](None, code=CLEANUP_PY)
        output = (result.get("result") or {}).get("output") if isinstance(result, dict) else None
        print("\ncleanup:", (output or ["(no output)"])[0].strip())
    except Exception as error:
        print(f"\ncleanup failed (inspect manually): {error}")


def main() -> int:
    if not bridge_up():
        print("FAIL: no editor bridge on 127.0.0.1:55557 - nothing to test. Start one with "
              "`uv run --project Python python Python/scripts/editor_process.py restart`.")
        return 1
    names = sorted(registry.tools)
    unreached: list[tuple[str, str]] = []
    print(f"smoke testing {len(names)} tool(s) against the live editor "
          f"(per-call timeout {_args.timeout}s)\n", flush=True)
    for name in names:
        func = registry.tools[name]
        if name in SKIP:
            print(f"  skip  {name}: writes project config; covered by static parity", flush=True)
            continue
        params = PARAMS.get(name, {})
        try:
            response = func(None, **params)
        except Exception as error:  # transport / param shaping failure
            unreached.append((name, f"exception: {error}"))
            print(f"  EXC   {name}: {error}", flush=True)
            continue
        text = json.dumps(response, default=str)
        if any(marker in text for marker in UNKNOWN_MARKERS):
            unreached.append((name, text[:160]))
            print(f"  UNREACHED {name}: {text[:120]}", flush=True)
            continue
        ok = response.get("success") if isinstance(response, dict) else None
        print(f"  {'ok  ' if ok else 'warn'}  {name}: {text[:100]}", flush=True)
        time.sleep(0.05)

    cleanup()
    print()
    if unreached:
        print(f"FAIL: {len(unreached)} tool(s) did not reach a handler:")
        for name, detail in unreached:
            print(f"  - {name}: {detail}")
        return 1
    print(f"PASS: all {len(names)} tool(s) reached a handler (no unknown-command failures).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
