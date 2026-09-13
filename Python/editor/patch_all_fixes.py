"""Apply the five fixes to the Unreal engine backend (both copies).

1. reload_server (idempotent state + stable tick trampoline)
2. delete_level + delete_actors_by_prefix (cleanup)
3. duplicate-label guard on all spawn handlers
4. capabilities derived from source via AST (no drift)
"""
from pathlib import Path

FILES = [
    Path('Python/mcp_unreal_engine.py'),
    Path('C:/Users/uraan/.reasonix/tools/unreal-mcp/Python/scripts/mcp_unreal_engine.py'),
]

PATCHES = []

# --- P1: imports + idempotent state + version bump ---
PATCHES.append((
'''import unreal
import socket
import json
import threading
import queue
import io
import sys
import os
import time
import traceback
from typing import Dict, Any, List, Optional

PORT = 55557
PROTOCOL_VERSION = "1.2.0"

_request_queue = queue.Queue()
_response_events = {}
_response_data = {}
_next_req_id = 1
_id_lock = threading.Lock()

# Server settings
ENABLE_PYTHON_EXECUTION = True
MAX_PYTHON_SCRIPT_LENGTH = 100000''',
'''import unreal
import ast
import socket
import json
import threading
import queue
import io
import sys
import os
import time
import traceback
from typing import Dict, Any, List, Optional

PORT = 55557
PROTOCOL_VERSION = "1.3.0"

# Server settings
ENABLE_PYTHON_EXECUTION = True
MAX_PYTHON_SCRIPT_LENGTH = 100000

# Live runtime state is created exactly once and PRESERVED across hot reloads,
# so reload_server never drops in-flight requests or the listener thread.
if "_request_queue" not in globals():
    _request_queue = queue.Queue()
    _response_events = {}
    _response_data = {}
    _next_req_id = 1
    _id_lock = threading.Lock()'''))

# --- P2: helpers before the dispatcher ---
PATCHES.append((
'''def on_slate_tick(delta_time):''',
'''def discover_supported_commands():
    """Derive the supported-command list from this file's own source so the
    capability handshake can never drift from the real dispatcher."""
    try:
        with open(__file__, "r", encoding="utf-8") as fh:
            tree = ast.parse(fh.read())
    except Exception:
        return []
    found = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Compare) and isinstance(node.left, ast.Name) and node.left.id == "cmd_type":
            for op, comp in zip(node.ops, node.comparators):
                if isinstance(op, ast.Eq) and isinstance(comp, ast.Constant) and isinstance(comp.value, str):
                    found.add(comp.value)
                elif isinstance(op, ast.In) and isinstance(comp, (ast.Tuple, ast.List, ast.Set)):
                    for el in comp.elts:
                        if isinstance(el, ast.Constant) and isinstance(el.value, str):
                            found.add(el.value)
    return sorted(found)


def find_label_conflict(label):
    """Return an existing actor whose label matches, or None."""
    if not label:
        return None
    for actor in unreal.EditorLevelLibrary.get_all_level_actors():
        if actor.get_actor_label() == label:
            return actor
    return None


def on_slate_tick(delta_time):'''))

# --- P3: capabilities derived, not hardcoded ---
PATCHES.append((
'''                        "supported_commands": [
                            "get_capabilities",
                            "execute_python",
                            "query_assets",
                            "get_asset_details",
                            "get_actors_in_level",
                            "get_actor_details",
                            "get_actor_properties",
                            "spawn_mesh_actor",
                            "spawn_actor",
                            "create_actor",
                            "spawn_blueprint_actor",
                            "set_actor_transform",
                            "delete_actor",
                            "batch_execute",
                            "set_viewport_camera",
                            "capture_viewport_screenshot",
                            "save_level",
                            "load_level",
                            "create_level",
                            "spawn_mesh_grid",
                            "spawn_instanced_mesh",
                            "spawn_light_actor",
                            "set_actor_folder",
                            "set_actor_material"
                        ],''',
'''                        "supported_commands": discover_supported_commands(),'''))

# --- P4: spawn_mesh_actor duplicate guard ---
PATCHES.append((
'''                mesh_asset = unreal.EditorAssetLibrary.load_asset(mesh_path)
                if not mesh_asset or not isinstance(mesh_asset, unreal.StaticMesh):
                    response = {"status": "error", "error": f"Failed to load static mesh asset: {mesh_path}"}
                else:
                    u_loc = unreal.Vector(loc[0], loc[1], loc[2])
                    u_rot = unreal.Rotator(rot[0], rot[1], rot[2])
                    u_scale = unreal.Vector(scale[0], scale[1], scale[2])

                    actor = unreal.EditorLevelLibrary.spawn_actor_from_class(unreal.StaticMeshActor, u_loc, u_rot)''',
'''                mesh_asset = unreal.EditorAssetLibrary.load_asset(mesh_path)
                if find_label_conflict(actor_name) and not params.get("allow_duplicate", False):
                    response = {"status": "error", "error": f"Actor label already in use: \'{actor_name}\'. Pass allow_duplicate=True to override."}
                elif not mesh_asset or not isinstance(mesh_asset, unreal.StaticMesh):
                    response = {"status": "error", "error": f"Failed to load static mesh asset: {mesh_path}"}
                else:
                    u_loc = unreal.Vector(loc[0], loc[1], loc[2])
                    u_rot = unreal.Rotator(rot[0], rot[1], rot[2])
                    u_scale = unreal.Vector(scale[0], scale[1], scale[2])

                    actor = unreal.EditorLevelLibrary.spawn_actor_from_class(unreal.StaticMeshActor, u_loc, u_rot)'''))

# --- P5: spawn_actor duplicate guard ---
PATCHES.append((
'''                cls = cls_map.get(actor_type)
                if cls:
                    actor = unreal.EditorLevelLibrary.spawn_actor_from_class(cls, u_loc, u_rot)''',
'''                cls = cls_map.get(actor_type)
                if find_label_conflict(name) and not params.get("allow_duplicate", False):
                    response = {"status": "error", "error": f"Actor label already in use: \'{name}\'. Pass allow_duplicate=True to override."}
                elif cls:
                    actor = unreal.EditorLevelLibrary.spawn_actor_from_class(cls, u_loc, u_rot)'''))

# --- P6: spawn_blueprint_actor duplicate guard ---
PATCHES.append((
'''                if not bp_class:
                    response = {"status": "error", "error": f"Blueprint class not found: {bp_path}"}
                else:
                    u_loc = unreal.Vector(loc[0], loc[1], loc[2])
                    u_rot = unreal.Rotator(rot[0], rot[1], rot[2])
                    actor = unreal.EditorLevelLibrary.spawn_actor_from_class(bp_class, u_loc, u_rot)''',
'''                if find_label_conflict(name) and not params.get("allow_duplicate", False):
                    response = {"status": "error", "error": f"Actor label already in use: \'{name}\'. Pass allow_duplicate=True to override."}
                elif not bp_class:
                    response = {"status": "error", "error": f"Blueprint class not found: {bp_path}"}
                else:
                    u_loc = unreal.Vector(loc[0], loc[1], loc[2])
                    u_rot = unreal.Rotator(rot[0], rot[1], rot[2])
                    actor = unreal.EditorLevelLibrary.spawn_actor_from_class(bp_class, u_loc, u_rot)'''))

# --- P7: instanced duplicate guard (raise inside try => rollback) ---
PATCHES.append((
'''                    actor = None
                    try:
                        with unreal.ScopedEditorTransaction(f"Spawn Instanced Mesh {actor_name}"):''',
'''                    actor = None
                    try:
                        if find_label_conflict(actor_name) and not params.get("allow_duplicate", False):
                            raise RuntimeError(f"Actor label already in use: \'{actor_name}\'. Pass allow_duplicate=True to override.")
                        with unreal.ScopedEditorTransaction(f"Spawn Instanced Mesh {actor_name}"):'''))

# --- P8: light duplicate guard (no reindent of the else-body) ---
PATCHES.append((
'''                actor = unreal.EditorLevelLibrary.spawn_actor_from_class(cls, u_loc, u_rot)
                if not actor:
                    response = {"status": "error", "error": f"Failed to spawn {light_type}"}
                else:
                    actor.set_actor_label(name)''',
'''                _name_conflict = bool(find_label_conflict(name)) and not params.get("allow_duplicate", False)
                actor = None if _name_conflict else unreal.EditorLevelLibrary.spawn_actor_from_class(cls, u_loc, u_rot)
                if _name_conflict:
                    response = {"status": "error", "error": f"Actor label already in use: \'{name}\'. Pass allow_duplicate=True to override."}
                elif not actor:
                    response = {"status": "error", "error": f"Failed to spawn {light_type}"}
                else:
                    actor.set_actor_label(name)'''))

# --- P9: new handlers before the final else ---
PATCHES.append((
'''            else:
                response = {"status": "error", "error": f"Unknown command: {cmd_type}"}''',
'''            # 16. Hot reload of this server module
            elif cmd_type == "reload_server":
                src_path = globals().get("__file__")
                if not src_path or not os.path.exists(src_path):
                    response = {"status": "error", "error": f"Cannot locate server source: {src_path}"}
                else:
                    try:
                        with open(src_path, "r", encoding="utf-8") as fh:
                            source = fh.read()
                        exec(compile(source, src_path, "exec"), globals())
                        response = {
                            "status": "success",
                            "result": {
                                "reloaded": True,
                                "source": src_path,
                                "version": globals().get("PROTOCOL_VERSION", "unknown"),
                                "commands": discover_supported_commands()
                            }
                        }
                    except Exception as reload_err:
                        response = {
                            "status": "error",
                            "error": f"Reload failed: {reload_err}",
                            "traceback": traceback.format_exc()
                        }

            # 17. Delete a level asset (with active-level protection)
            elif cmd_type == "delete_level":
                map_path = params.get("map_path", "")
                force = params.get("force", False)
                if not map_path:
                    response = {"status": "error", "error": "map_path is required"}
                elif not unreal.EditorAssetLibrary.does_asset_exist(map_path):
                    response = {"status": "error", "error": f"Level asset not found: {map_path}"}
                else:
                    active = unreal.EditorLevelLibrary.get_editor_world()
                    active_name = active.get_path_name() if active else ""
                    if active_name.startswith(map_path) and not force:
                        response = {"status": "error", "error": f"Refusing to delete the currently open level: {map_path}. Pass force=True to override."}
                    else:
                        deleted = unreal.EditorAssetLibrary.delete_asset(map_path)
                        response = {
                            "status": "success" if deleted else "error",
                            "result": {"deleted": bool(deleted), "map_path": map_path}
                        }

            # 18. Bulk cleanup of actors by label prefix
            elif cmd_type == "delete_actors_by_prefix":
                prefix = params.get("prefix", "")
                if not prefix:
                    response = {"status": "error", "error": "prefix is required"}
                else:
                    to_delete = [act for act in unreal.EditorLevelLibrary.get_all_level_actors()
                                 if act.get_actor_label().startswith(prefix)]
                    deleted_names = []
                    with unreal.ScopedEditorTransaction(f"Delete actors prefixed \'{prefix}\'"):
                        for act in to_delete:
                            deleted_names.append(act.get_actor_label())
                            unreal.EditorLevelLibrary.destroy_actor(act)
                    response = {
                        "status": "success",
                        "result": {"deleted_count": len(deleted_names), "deleted": deleted_names[:100], "prefix": prefix}
                    }

            else:
                response = {"status": "error", "error": f"Unknown command: {cmd_type}"}'''))

# --- P10: stable trampoline + idempotent start ---
PATCHES.append((
'''def start():
    unreal.register_slate_post_tick_callback(on_slate_tick)
    t = threading.Thread(target=listener_worker, daemon=True)
    t.start()
    unreal.log_warning("MCP Unreal Engine Python Server initialized successfully!")''',
'''def _tick_trampoline(delta_time):
    # Stable Slate callback: always dispatches to the CURRENT on_slate_tick held in
    # module globals, so a hot reload swaps the handler without re-registering.
    fn = globals().get("on_slate_tick")
    if fn is not None:
        fn(delta_time)


def start():
    if globals().get("_listener_started"):
        unreal.log_warning("MCP server already running; on_slate_tick reloaded in place.")
        return
    globals()["_listener_started"] = True
    globals()["_tick_handle"] = unreal.register_slate_post_tick_callback(_tick_trampoline)
    t = threading.Thread(target=listener_worker, daemon=True)
    t.start()
    unreal.log_warning("MCP Unreal Engine Python Server initialized successfully!")'''))

for f in FILES:
    text = f.read_text(encoding='utf-8')
    for idx, (old, new) in enumerate(PATCHES):
        assert old in text, f'PATCH {idx} not found in {f}'
        assert text.count(old) == 1, f'PATCH {idx} not unique in {f}'
        text = text.replace(old, new)
    f.write_text(text, encoding='utf-8')
    print(f'Applied {len(PATCHES)} patches to {f}')
