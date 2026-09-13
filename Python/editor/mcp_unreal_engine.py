"""
Unreal Engine Python Server for MCP Integration
Supports capabilities handshake, asset inspection, actor inspection,
transactional batch execution, viewport capture, and map management.
"""

import unreal
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
    _id_lock = threading.Lock()

def get_next_id():
    global _next_req_id
    with _id_lock:
        req_id = _next_req_id
        _next_req_id += 1
        return req_id

def serialize_vector(v: unreal.Vector) -> List[float]:
    return [round(v.x, 3), round(v.y, 3), round(v.z, 3)]

def serialize_rotator(r: unreal.Rotator) -> List[float]:
    return [round(r.pitch, 3), round(r.yaw, 3), round(r.roll, 3)]

def get_actor_by_identifier(identifier: str) -> Optional[unreal.Actor]:
    """Find actor by path name first (guaranteed unique), then by label, then by name."""
    actors = unreal.EditorLevelLibrary.get_all_level_actors()
    for a in actors:
        if a.get_path_name() == identifier:
            return a
    for a in actors:
        if a.get_actor_label() == identifier:
            return a
    for a in actors:
        if a.get_name() == identifier:
            return a
    return None

def inspect_actor_data(actor: unreal.Actor) -> Dict[str, Any]:
    loc = actor.get_actor_location()
    rot = actor.get_actor_rotation()
    scale = actor.get_actor_scale3d()
    bounds_origin, bounds_extent = actor.get_actor_bounds(False)
    
    components_info = []
    static_mesh_path = None
    materials = []
    
    for comp in actor.get_components_by_class(unreal.ActorComponent):
        comp_data = {
            "name": comp.get_name(),
            "class": comp.get_class().get_name()
        }
        if isinstance(comp, unreal.StaticMeshComponent):
            mesh = comp.static_mesh
            if mesh:
                static_mesh_path = mesh.get_path_name()
                comp_data["mesh"] = static_mesh_path
                mat_count = comp.get_num_materials()
                for mi in range(mat_count):
                    m = comp.get_material(mi)
                    if m:
                        materials.append({"slot": mi, "material": m.get_path_name()})
        elif isinstance(comp, unreal.LightComponent):
            comp_data["intensity"] = comp.get_editor_property("intensity") if hasattr(comp, "get_editor_property") else None
            light_color = comp.get_editor_property("light_color") if hasattr(comp, "get_editor_property") else None
            if light_color:
                comp_data["color"] = [light_color.r, light_color.g, light_color.b, light_color.a]
        components_info.append(comp_data)
        
    return {
        "name": actor.get_name(),
        "label": actor.get_actor_label(),
        "path": actor.get_path_name(),
        "class": actor.get_class().get_name(),
        "folder": str(actor.get_folder_path()) if hasattr(actor, "get_folder_path") else "",
        "location": serialize_vector(loc),
        "rotation": serialize_rotator(rot),
        "scale": serialize_vector(scale),
        "bounds": {
            "origin": serialize_vector(bounds_origin),
            "extent": serialize_vector(bounds_extent)
        },
        "static_mesh": static_mesh_path,
        "materials": materials,
        "components": components_info
    }

def discover_supported_commands():
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


def on_slate_tick(delta_time):
    while not _request_queue.empty():
        try:
            req_id, cmd_type, params, enqueued_time = _request_queue.get_nowait()
        except queue.Empty:
            break
        
        # Timeout safeguard: if request stayed queued > 25 seconds before thread could handle it, abort
        if time.time() - enqueued_time > 25.0:
            _response_data[req_id] = {
                "status": "error",
                "error": "Request timed out in queue before processing started"
            }
            if req_id in _response_events:
                _response_events[req_id].set()
            continue

        response = {"status": "error", "error": "Unknown error"}
        try:
            # 1. Capability Handshake
            if cmd_type in ("get_capabilities", "handshake"):
                response = {
                    "status": "success",
                    "result": {
                        "version": PROTOCOL_VERSION,
                        "engine_version": unreal.SystemLibrary.get_engine_version(),
                        "supported_commands": discover_supported_commands(),
                        "features": {
                            "python_execution": ENABLE_PYTHON_EXECUTION,
                            "transactions": True,
                            "asset_discovery": True,
                            "viewport_capture": True,
                            "map_lifecycle": True,
                            "pagination": True
                        }
                    }
                }

            # 2. Hardened Python execution
            elif cmd_type == "execute_python":
                if not ENABLE_PYTHON_EXECUTION:
                    response = {"status": "error", "error": "Python execution is disabled by server policy"}
                else:
                    code = params.get("code", "")
                    if len(code) > MAX_PYTHON_SCRIPT_LENGTH:
                        response = {"status": "error", "error": f"Script length exceeds maximum permitted ({MAX_PYTHON_SCRIPT_LENGTH} chars)"}
                    else:
                        buffer = io.StringIO()
                        old_stdout = sys.stdout
                        sys.stdout = buffer
                        try:
                            exec_globals = {"unreal": unreal}
                            exec(code, exec_globals)
                            output = buffer.getvalue()
                            response = {"status": "success", "result": output}
                        finally:
                            sys.stdout = old_stdout

            # 3. Asset Query & Inspection
            elif cmd_type == "query_assets":
                path = params.get("path", "/Game")
                recursive = params.get("recursive", True)
                asset_class = params.get("asset_class")
                search_term = params.get("search", "").lower()
                limit = max(1, min(params.get("limit", 100), 500))
                offset = max(0, params.get("offset", 0))

                all_assets = unreal.EditorAssetLibrary.list_assets(path, recursive=recursive, include_folder=False)
                
                filtered = []
                for a_path in all_assets:
                    if search_term and search_term not in a_path.lower():
                        continue
                    if asset_class:
                        asset_data = unreal.EditorAssetLibrary.find_asset_data(a_path)
                        if not asset_data:
                            continue
                        cls_name = str(asset_data.asset_class_path.asset_name) if hasattr(asset_data, "asset_class_path") else str(asset_data.asset_class)
                        if cls_name.lower() != asset_class.lower():
                            continue
                    filtered.append(a_path)

                total_count = len(filtered)
                paged = filtered[offset:offset + limit]

                response = {
                    "status": "success",
                    "result": {
                        "total": total_count,
                        "offset": offset,
                        "limit": limit,
                        "assets": paged
                    }
                }

            elif cmd_type == "get_asset_details":
                asset_path = params.get("asset_path", "")
                asset = unreal.EditorAssetLibrary.load_asset(asset_path)
                if not asset:
                    response = {"status": "error", "error": f"Asset not found: {asset_path}"}
                else:
                    details = {
                        "path": asset_path,
                        "name": asset.get_name(),
                        "class": asset.get_class().get_name()
                    }
                    if isinstance(asset, unreal.StaticMesh):
                        box = asset.get_bounding_box()
                        bounds = {
                            "min": serialize_vector(box.min),
                            "max": serialize_vector(box.max),
                            "size": [
                                round(box.max.x - box.min.x, 3),
                                round(box.max.y - box.min.y, 3),
                                round(box.max.z - box.min.z, 3)
                            ]
                        }
                        materials = []
                        for mat in asset.static_materials:
                            materials.append({
                                "slot_name": str(mat.material_slot_name),
                                "interface": mat.material_interface.get_path_name() if mat.material_interface else None
                            })
                        details["bounding_box"] = bounds
                        details["materials"] = materials
                    response = {"status": "success", "result": details}

            # 4. Deep Actor Inspection
            elif cmd_type in ("get_actor_details", "get_actor_properties"):
                name = params.get("name", "")
                actor = get_actor_by_identifier(name)
                if not actor:
                    response = {"status": "error", "error": f"Actor '{name}' not found in active level"}
                else:
                    response = {
                        "status": "success",
                        "result": inspect_actor_data(actor)
                    }

            # 5. Actors in Level (bounded and filterable)
            elif cmd_type == "get_actors_in_level":
                class_filter = params.get("class_filter")
                search_term = params.get("search", "").lower()
                limit = max(1, min(params.get("limit", 200), 1000))
                offset = max(0, params.get("offset", 0))

                all_actors = unreal.EditorLevelLibrary.get_all_level_actors()
                filtered = []
                for a in all_actors:
                    label = a.get_actor_label()
                    cls_name = a.get_class().get_name()
                    if class_filter and cls_name.lower() != class_filter.lower():
                        continue
                    if search_term and search_term not in label.lower():
                        continue
                    loc = a.get_actor_location()
                    rot = a.get_actor_rotation()
                    scale = a.get_actor_scale3d()
                    filtered.append({
                        "label": label,
                        "name": a.get_name(),
                        "path": a.get_path_name(),
                        "class": cls_name,
                        "location": serialize_vector(loc),
                        "rotation": serialize_rotator(rot),
                        "scale": serialize_vector(scale)
                    })

                total = len(filtered)
                paged = filtered[offset:offset + limit]
                response = {
                    "status": "success",
                    "result": {
                        "total": total,
                        "offset": offset,
                        "limit": limit,
                        "actors": paged
                    }
                }

            # 6. Spawning & Modification
            elif cmd_type == "spawn_mesh_actor":
                mesh_path = params.get("mesh_path", "")
                actor_name = params.get("name", "MeshActor")
                loc = params.get("location", [0, 0, 0])
                rot = params.get("rotation", [0, 0, 0])
                scale = params.get("scale", [1, 1, 1])

                mesh_asset = unreal.EditorAssetLibrary.load_asset(mesh_path)
                if find_label_conflict(actor_name) and not params.get("allow_duplicate", False):
                    response = {"status": "error", "error": f"Actor label already in use: '{actor_name}'. Pass allow_duplicate=True to override."}
                elif not mesh_asset or not isinstance(mesh_asset, unreal.StaticMesh):
                    response = {"status": "error", "error": f"Failed to load static mesh asset: {mesh_path}"}
                else:
                    u_loc = unreal.Vector(loc[0], loc[1], loc[2])
                    u_rot = unreal.Rotator(rot[0], rot[1], rot[2])
                    u_scale = unreal.Vector(scale[0], scale[1], scale[2])

                    actor = unreal.EditorLevelLibrary.spawn_actor_from_class(unreal.StaticMeshActor, u_loc, u_rot)
                    if actor:
                        actor.set_actor_label(actor_name)
                        folder_param = params.get("folder_path")
                        if folder_param:
                            actor.set_folder_path(folder_param)
                        mesh_comp = actor.get_component_by_class(unreal.StaticMeshComponent)
                        if mesh_comp:
                            mesh_comp.set_static_mesh(mesh_asset)
                        actor.set_actor_scale3d(u_scale)
                        response = {
                            "status": "success",
                            "result": inspect_actor_data(actor)
                        }
                    else:
                        response = {"status": "error", "error": "Failed to spawn static mesh actor"}

            elif cmd_type in ("spawn_actor", "create_actor"):
                actor_type = params.get("type", "").lower()
                name = params.get("name", "Actor")
                loc = params.get("location", [0, 0, 0])
                rot = params.get("rotation", [0, 0, 0])
                scale = params.get("scale", [1, 1, 1])

                u_loc = unreal.Vector(loc[0], loc[1], loc[2])
                u_rot = unreal.Rotator(rot[0], rot[1], rot[2])
                u_scale = unreal.Vector(scale[0], scale[1], scale[2])

                cls_map = {
                    "staticmeshactor": unreal.StaticMeshActor,
                    "mesh": unreal.StaticMeshActor,
                    "pointlight": unreal.PointLight,
                    "point_light": unreal.PointLight,
                    "directionallight": unreal.DirectionalLight,
                    "directional_light": unreal.DirectionalLight,
                    "spotlight": unreal.SpotLight,
                    "spot_light": unreal.SpotLight,
                    "rectlight": unreal.RectLight,
                    "rect_light": unreal.RectLight,
                    "skylight": unreal.SkyLight,
                    "sky_light": unreal.SkyLight,
                    "playerstart": unreal.PlayerStart,
                    "player_start": unreal.PlayerStart,
                    "cameraactor": unreal.CameraActor,
                    "camera": unreal.CameraActor,
                    "postprocessvolume": unreal.PostProcessVolume
                }

                cls = cls_map.get(actor_type)
                if find_label_conflict(name) and not params.get("allow_duplicate", False):
                    response = {"status": "error", "error": f"Actor label already in use: '{name}'. Pass allow_duplicate=True to override."}
                elif cls:
                    actor = unreal.EditorLevelLibrary.spawn_actor_from_class(cls, u_loc, u_rot)
                    if actor:
                        actor.set_actor_label(name)
                        folder_param = params.get("folder_path")
                        if folder_param:
                            actor.set_folder_path(folder_param)
                        actor.set_actor_scale3d(u_scale)
                        response = {
                            "status": "success",
                            "result": inspect_actor_data(actor)
                        }
                    else:
                        response = {"status": "error", "error": f"Failed to spawn {actor_type}"}
                else:
                    response = {"status": "error", "error": f"Unsupported actor type: {actor_type}"}

            elif cmd_type == "spawn_blueprint_actor":
                bp_name = params.get("blueprint_name", "")
                name = params.get("actor_name", "BPActor")
                loc = params.get("location", [0, 0, 0])
                rot = params.get("rotation", [0, 0, 0])

                bp_path = bp_name if bp_name.startswith("/") else f"/Game/Blueprints/{bp_name}"
                bp_class = unreal.EditorAssetLibrary.load_blueprint_class(bp_path)
                if not bp_class and not bp_path.endswith("_C"):
                    bp_class = unreal.EditorAssetLibrary.load_blueprint_class(f"{bp_path}.{bp_path.split('/')[-1]}_C")

                if find_label_conflict(name) and not params.get("allow_duplicate", False):
                    response = {"status": "error", "error": f"Actor label already in use: '{name}'. Pass allow_duplicate=True to override."}
                elif not bp_class:
                    response = {"status": "error", "error": f"Blueprint class not found: {bp_path}"}
                else:
                    u_loc = unreal.Vector(loc[0], loc[1], loc[2])
                    u_rot = unreal.Rotator(rot[0], rot[1], rot[2])
                    actor = unreal.EditorLevelLibrary.spawn_actor_from_class(bp_class, u_loc, u_rot)
                    if actor:
                        actor.set_actor_label(name)
                        response = {
                            "status": "success",
                            "result": inspect_actor_data(actor)
                        }
                    else:
                        response = {"status": "error", "error": "Failed to spawn Blueprint actor"}

            elif cmd_type == "delete_actor":
                name = params.get("name", "")
                actor = get_actor_by_identifier(name)
                if actor:
                    deleted_id = actor.get_actor_label()
                    unreal.EditorLevelLibrary.destroy_actor(actor)
                    response = {"status": "success", "result": {"deleted": deleted_id}}
                else:
                    response = {"status": "error", "error": f"Actor '{name}' not found"}

            elif cmd_type == "set_actor_transform":
                name = params.get("name", "")
                loc = params.get("location")
                rot = params.get("rotation")
                scale = params.get("scale")

                actor = get_actor_by_identifier(name)
                if actor:
                    if loc is not None:
                        actor.set_actor_location(unreal.Vector(loc[0], loc[1], loc[2]), False, False)
                    if rot is not None:
                        actor.set_actor_rotation(unreal.Rotator(rot[0], rot[1], rot[2]), False)
                    if scale is not None:
                        actor.set_actor_scale3d(unreal.Vector(scale[0], scale[1], scale[2]))
                    response = {"status": "success", "result": inspect_actor_data(actor)}
                else:
                    response = {"status": "error", "error": f"Actor '{name}' not found"}

            # 7. Transactional Batch Operations with Rollback
            elif cmd_type == "batch_execute":
                actions = params.get("actions", [])
                description = params.get("description", "MCP Batch Operation")
                rollback_on_failure = params.get("rollback_on_failure", True)

                results = []
                failed = False
                failure_reason = None

                # Execute inside an explicit editor transaction
                with unreal.ScopedEditorTransaction(description) as trans:
                    for idx, action in enumerate(actions):
                        action_type = action.get("action")
                        action_params = action.get("params", {})
                        
                        try:
                            if action_type == "spawn_mesh_actor":
                                mesh_asset = unreal.EditorAssetLibrary.load_asset(action_params.get("mesh_path", ""))
                                if not mesh_asset:
                                    raise RuntimeError(f"Mesh not found: {action_params.get('mesh_path')}")
                                loc = action_params.get("location", [0, 0, 0])
                                rot = action_params.get("rotation", [0, 0, 0])
                                scale = action_params.get("scale", [1, 1, 1])
                                a = unreal.EditorLevelLibrary.spawn_actor_from_class(
                                    unreal.StaticMeshActor,
                                    unreal.Vector(loc[0], loc[1], loc[2]),
                                    unreal.Rotator(rot[0], rot[1], rot[2])
                                )
                                a.set_actor_label(action_params.get("name", "MeshActor"))
                                a.static_mesh_component.set_static_mesh(mesh_asset)
                                a.set_actor_scale3d(unreal.Vector(scale[0], scale[1], scale[2]))
                                results.append({"index": idx, "success": True, "actor": a.get_actor_label()})

                            elif action_type == "set_actor_transform":
                                a = get_actor_by_identifier(action_params.get("name", ""))
                                if not a:
                                    raise RuntimeError(f"Actor not found: {action_params.get('name')}")
                                if "location" in action_params:
                                    l = action_params["location"]
                                    a.set_actor_location(unreal.Vector(l[0], l[1], l[2]), False, False)
                                if "rotation" in action_params:
                                    r = action_params["rotation"]
                                    a.set_actor_rotation(unreal.Rotator(r[0], r[1], r[2]), False)
                                if "scale" in action_params:
                                    s = action_params["scale"]
                                    a.set_actor_scale3d(unreal.Vector(s[0], s[1], s[2]))
                                results.append({"index": idx, "success": True, "actor": a.get_actor_label()})

                            elif action_type == "delete_actor":
                                a = get_actor_by_identifier(action_params.get("name", ""))
                                if not a:
                                    raise RuntimeError(f"Actor not found: {action_params.get('name')}")
                                lbl = a.get_actor_label()
                                unreal.EditorLevelLibrary.destroy_actor(a)
                                results.append({"index": idx, "success": True, "deleted": lbl})

                            else:
                                raise RuntimeError(f"Unsupported batch action: {action_type}")

                        except Exception as action_err:
                            failed = True
                            failure_reason = f"Action {idx} ({action_type}) failed: {str(action_err)}"
                            results.append({"index": idx, "success": False, "error": str(action_err)})
                            break

                    if failed and rollback_on_failure:
                        trans.cancel()
                        response = {
                            "status": "error",
                            "error": f"Batch transaction cancelled due to failure: {failure_reason}",
                            "result": {"rolled_back": True, "actions": results}
                        }
                    else:
                        response = {
                            "status": "success" if not failed else "partial",
                            "result": {"rolled_back": False, "actions": results}
                        }

            # 8. Viewport Camera & Screenshot
            elif cmd_type == "set_viewport_camera":
                loc = params.get("location", [0, 0, 0])
                rot = params.get("rotation", [0, 0, 0])
                game_view = params.get("game_view", False)

                unreal.EditorLevelLibrary.set_level_viewport_camera_info(
                    unreal.Vector(loc[0], loc[1], loc[2]),
                    unreal.Rotator(rot[0], rot[1], rot[2])
                )
                if game_view is not None:
                    unreal.EditorLevelLibrary.editor_set_game_view(game_view)
                response = {
                    "status": "success",
                    "result": {
                        "location": loc,
                        "rotation": rot,
                        "game_view": game_view
                    }
                }

            elif cmd_type == "capture_viewport_screenshot":
                filename = params.get("filename", "MCP_Screenshot.png")
                width = params.get("width", 1440)
                height = params.get("height", 1000)
                
                # Determine absolute target path
                if not os.path.isabs(filename):
                    content_dir = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_content_dir())
                    out_path = os.path.join(content_dir, filename)
                else:
                    out_path = filename
                
                # Normalize path
                out_path = os.path.normpath(out_path)
                
                # Remove prior file if exists
                if os.path.exists(out_path):
                    try:
                        os.remove(out_path)
                    except:
                        pass
                
                unreal.AutomationLibrary.take_high_res_screenshot(width, height, out_path)
                
                response = {
                    "status": "success",
                    "result": {
                        "requested_path": out_path,
                        "file_name": os.path.basename(out_path),
                        "dimensions": [width, height],
                        "exists_immediately": os.path.exists(out_path),
                        "message": "Screenshot task scheduled. The file is written by automation capture."
                    }
                }

            # 9. Map Lifecycle Management
            elif cmd_type == "save_level":
                destination_path = params.get("destination_path")
                overwrite = params.get("overwrite", False)

                world = unreal.EditorLevelLibrary.get_editor_world()
                if destination_path:
                    if not overwrite and unreal.EditorAssetLibrary.does_asset_exist(destination_path):
                        response = {
                            "status": "error",
                            "error": f"Destination map asset already exists and overwrite is False: {destination_path}"
                        }
                    else:
                        saved = unreal.EditorLoadingAndSavingUtils.save_map(world, destination_path)
                        response = {
                            "status": "success" if saved else "error",
                            "result": {
                                "saved": saved,
                                "destination_path": destination_path,
                                "world": world.get_path_name()
                            }
                        }
                else:
                    saved = unreal.EditorLoadingAndSavingUtils.save_current_level()
                    response = {
                        "status": "success" if saved else "error",
                        "result": {
                            "saved": saved,
                            "world": world.get_path_name()
                        }
                    }

            elif cmd_type == "load_level":
                map_path = params.get("map_path", "")
                if not unreal.EditorAssetLibrary.does_asset_exist(map_path):
                    response = {"status": "error", "error": f"Map asset not found: {map_path}"}
                else:
                    loaded_world = unreal.EditorLoadingAndSavingUtils.load_map(map_path)
                    response = {
                        "status": "success" if loaded_world else "error",
                        "result": {
                            "loaded": loaded_world is not None,
                            "active_world": loaded_world.get_path_name() if loaded_world else None
                        }
                    }


            # 10. Explicit Level Creation
            elif cmd_type == "create_level":
                map_path = params.get("map_path", "")
                template = params.get("template", "empty")
                overwrite = params.get("overwrite", False)

                if not map_path:
                    response = {"status": "error", "error": "map_path is required"}
                elif not overwrite and unreal.EditorAssetLibrary.does_asset_exist(map_path):
                    response = {"status": "error", "error": f"Level asset already exists and overwrite is False: {map_path}"}
                else:
                    # new_level opens a fresh canvas; persist and verify it lands at map_path.
                    try:
                        unreal.EditorLevelLibrary.new_level(map_path)
                    except Exception:
                        pass
                    active_w = unreal.EditorLevelLibrary.get_editor_world()
                    active_name = active_w.get_path_name() if active_w else ""

                    if not active_name.startswith(map_path):
                        if not unreal.EditorLoadingAndSavingUtils.save_map(active_w, map_path):
                            response = {"status": "error", "error": f"Failed to save new level to {map_path}"}
                            active_w = None
                        else:
                            active_w = unreal.EditorLoadingAndSavingUtils.load_map(map_path)

                    if active_w is not None:
                        final_name = active_w.get_path_name() if active_w else ""
                        if final_name.startswith(map_path):
                            response = {
                                "status": "success",
                                "result": {
                                    "created": True,
                                    "map_path": map_path,
                                    "active_world": final_name
                                }
                            }
                        else:
                            response = {
                                "status": "error",
                                "error": f"Level creation did not yield the requested world. Active: {final_name}"
                            }

            # 11. High-Throughput Mesh Grid Spawning
            elif cmd_type == "spawn_mesh_grid":
                mesh_path = params.get("mesh_path", "")
                rows = max(1, params.get("rows", 1))
                cols = max(1, params.get("cols", 1))
                spacing_x = params.get("spacing_x", 200.0)
                spacing_y = params.get("spacing_y", 200.0)
                origin = params.get("origin", [0.0, 0.0, 0.0])
                rot = params.get("rotation", [0.0, 0.0, 0.0])
                scale = params.get("scale", [1.0, 1.0, 1.0])
                prefix = params.get("prefix", "GridActor")
                folder = params.get("folder_path", "Environment/Grids")

                mesh_asset = unreal.EditorAssetLibrary.load_asset(mesh_path)
                if not mesh_asset or not isinstance(mesh_asset, unreal.StaticMesh):
                    response = {"status": "error", "error": f"Invalid StaticMesh asset: {mesh_path}"}
                else:
                    spawned = []
                    u_scale = unreal.Vector(scale[0], scale[1], scale[2])
                    u_rot = unreal.Rotator(pitch=rot[0], yaw=rot[1], roll=rot[2])

                    with unreal.ScopedEditorTransaction(f"Spawn Mesh Grid {prefix}") as trans:
                        for r in range(rows):
                            for c in range(cols):
                                loc_x = origin[0] + (r * spacing_x)
                                loc_y = origin[1] + (c * spacing_y)
                                loc_z = origin[2]
                                u_loc = unreal.Vector(loc_x, loc_y, loc_z)

                                actor = unreal.EditorLevelLibrary.spawn_actor_from_class(unreal.StaticMeshActor, u_loc, u_rot)
                                if actor:
                                    lbl = f"{prefix}_{r:02d}_{c:02d}"
                                    actor.set_actor_label(lbl)
                                    if folder:
                                        actor.set_folder_path(folder)
                                    actor.static_mesh_component.set_static_mesh(mesh_asset)
                                    actor.set_actor_scale3d(u_scale)
                                    spawned.append(lbl)

                    response = {
                        "status": "success",
                        "result": {
                            "mesh": mesh_path,
                            "count": len(spawned),
                            "rows": rows,
                            "cols": cols,
                            "folder": folder,
                            "actors": spawned[:50]
                        }
                    }

            # 12. Instanced Mesh Spawning (HISM - one actor holding many instances)
            elif cmd_type == "spawn_instanced_mesh":
                mesh_path = params.get("mesh_path", "")
                actor_name = params.get("name", "InstancedActor")
                folder = params.get("folder_path", "Environment/Instances")
                instances = params.get("instances", [])

                mesh_asset = unreal.EditorAssetLibrary.load_asset(mesh_path)
                if not mesh_asset or not isinstance(mesh_asset, unreal.StaticMesh):
                    response = {"status": "error", "error": f"Invalid StaticMesh asset: {mesh_path}"}
                elif not instances:
                    response = {"status": "error", "error": "instances list is empty; nothing to spawn"}
                else:
                    actor = None
                    try:
                        if find_label_conflict(actor_name) and not params.get("allow_duplicate", False):
                            raise RuntimeError(f"Actor label already in use: '{actor_name}'. Pass allow_duplicate=True to override.")
                        with unreal.ScopedEditorTransaction(f"Spawn Instanced Mesh {actor_name}"):
                            actor = unreal.EditorLevelLibrary.spawn_actor_from_class(
                                unreal.StaticMeshActor,
                                unreal.Vector(0, 0, 0),
                                unreal.Rotator(0, 0, 0)
                            )
                            actor.set_actor_label(actor_name)
                            if folder:
                                actor.set_folder_path(folder)

                            # Neutralize the default component so only the HISM renders.
                            base_comp = actor.static_mesh_component
                            if base_comp:
                                base_comp.set_static_mesh(None)

                            # Add a HierarchicalInstancedStaticMeshComponent via the subobject subsystem.
                            sds = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
                            if not sds:
                                raise RuntimeError("SubobjectDataSubsystem unavailable; cannot create HISM")
                            roots = sds.k2_gather_subobject_data_for_instance(actor)
                            if not roots:
                                raise RuntimeError("No root subobject handle for spawned actor")
                            add_params = unreal.AddNewSubobjectParams(
                                parent_handle=roots[0],
                                new_class=unreal.HierarchicalInstancedStaticMeshComponent,
                                blueprint_context=None
                            )
                            # NOTE: add_new_subobject returns (handle, Text("")); the Text is
                            # truthy even when empty, so success is confirmed by the component lookup below.
                            sds.add_new_subobject(add_params)

                            hism = None
                            for c in actor.get_components_by_class(unreal.InstancedStaticMeshComponent):
                                hism = c
                                break
                            if hism is None:
                                raise RuntimeError("HISM component not present after creation")

                            hism.set_static_mesh(mesh_asset)

                            transforms = []
                            for inst in instances:
                                loc = inst.get("location", [0, 0, 0])
                                r = inst.get("rotation", [0, 0, 0])
                                s = inst.get("scale", [1, 1, 1])
                                transforms.append(unreal.Transform(
                                    location=unreal.Vector(loc[0], loc[1], loc[2]),
                                    rotation=unreal.Rotator(pitch=r[0], yaw=r[1], roll=r[2]),
                                    scale=unreal.Vector(s[0], s[1], s[2])
                                ))
                            hism.add_instances(transforms, False)

                            # Read back the TRUE instance count; never trust the request.
                            real_count = hism.get_instance_count()
                            if real_count != len(instances):
                                raise RuntimeError(
                                    f"Instance count mismatch: requested {len(instances)}, created {real_count}"
                                )

                        response = {
                            "status": "success",
                            "result": {
                                "actor": actor.get_actor_label(),
                                "mesh": mesh_path,
                                "component_class": hism.get_class().get_name(),
                                "instance_count": real_count,
                                "folder": folder
                            }
                        }
                    except Exception as e:
                        # No half-built actor left behind on failure.
                        if actor:
                            try:
                                unreal.EditorLevelLibrary.destroy_actor(actor)
                            except Exception:
                                pass
                        response = {"status": "error", "error": str(e)}

            # 13. First-Class Light Spawning
            elif cmd_type == "spawn_light_actor":
                light_type = params.get("light_type", "PointLight").lower()
                name = params.get("name", "LightActor")
                loc = params.get("location", [0, 0, 0])
                rot = params.get("rotation", [0, 0, 0])
                intensity = float(params.get("intensity", 3000.0))
                color = params.get("color", [1.0, 1.0, 1.0])
                radius = float(params.get("attenuation_radius", 1000.0))
                source_radius = float(params.get("source_radius", 20.0))
                mobility_str = params.get("mobility", "movable").lower()
                folder = params.get("folder_path", "Environment/Lighting")
                warnings = []

                cls_map = {
                    "pointlight": unreal.PointLight,
                    "spotlight": unreal.SpotLight,
                    "rectlight": unreal.RectLight,
                    "directionallight": unreal.DirectionalLight
                }
                cls = cls_map.get(light_type, unreal.PointLight)

                u_loc = unreal.Vector(loc[0], loc[1], loc[2])
                u_rot = unreal.Rotator(pitch=rot[0], yaw=rot[1], roll=rot[2])

                _name_conflict = bool(find_label_conflict(name)) and not params.get("allow_duplicate", False)
                actor = None if _name_conflict else unreal.EditorLevelLibrary.spawn_actor_from_class(cls, u_loc, u_rot)
                if _name_conflict:
                    response = {"status": "error", "error": f"Actor label already in use: '{name}'. Pass allow_duplicate=True to override."}
                elif not actor:
                    response = {"status": "error", "error": f"Failed to spawn {light_type}"}
                else:
                    actor.set_actor_label(name)
                    if folder:
                        actor.set_folder_path(folder)

                    c = actor.get_component_by_class(unreal.LightComponent)
                    if c:
                        if mobility_str == "movable":
                            c.set_mobility(unreal.ComponentMobility.MOVABLE)
                        elif mobility_str == "stationary":
                            c.set_mobility(unreal.ComponentMobility.STATIONARY)
                        elif mobility_str == "static":
                            c.set_mobility(unreal.ComponentMobility.STATIC)

                        c.set_intensity(intensity)
                        col = unreal.LinearColor(color[0], color[1], color[2], 1.0)
                        c.set_light_color(col)

                        if hasattr(c, "set_attenuation_radius"):
                            c.set_attenuation_radius(radius)
                        if hasattr(c, "set_editor_property"):
                            try:
                                c.set_editor_property("source_radius", source_radius)
                            except Exception as _sr_err:
                                warnings.append(f"source_radius not applied: {_sr_err}")

                    light_result = inspect_actor_data(actor)
                    if warnings:
                        light_result["warnings"] = warnings
                    response = {
                        "status": "success",
                        "result": light_result
                    }

            # 14. Actor Folder Organization
            elif cmd_type == "set_actor_folder":
                name = params.get("name", "")
                folder = params.get("folder_path", "")

                actor = get_actor_by_identifier(name)
                if not actor:
                    response = {"status": "error", "error": f"Actor '{name}' not found"}
                else:
                    actor.set_folder_path(folder)
                    response = {
                        "status": "success",
                        "result": {
                            "actor": actor.get_actor_label(),
                            "folder_path": folder
                        }
                    }

            # 15. Material Assignment & Swapping
            elif cmd_type == "set_actor_material":
                name = params.get("name", "")
                slot = params.get("slot_index", 0)
                mat_path = params.get("material_path", "")

                actor = get_actor_by_identifier(name)
                if not actor:
                    response = {"status": "error", "error": f"Actor '{name}' not found"}
                else:
                    mat_asset = unreal.EditorAssetLibrary.load_asset(mat_path)
                    if not mat_asset:
                        response = {"status": "error", "error": f"Material asset not found: {mat_path}"}
                    else:
                        mesh_comp = actor.get_component_by_class(unreal.StaticMeshComponent)
                        if not mesh_comp:
                            response = {"status": "error", "error": f"Actor '{name}' has no StaticMeshComponent"}
                        else:
                            mesh_comp.set_material(slot, mat_asset)
                            response = {
                                "status": "success",
                                "result": {
                                    "actor": actor.get_actor_label(),
                                    "slot_index": slot,
                                    "material": mat_asset.get_path_name()
                                }
                            }

            # 16. Hot reload of this server module
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
                    with unreal.ScopedEditorTransaction(f"Delete actors prefixed '{prefix}'"):
                        for act in to_delete:
                            deleted_names.append(act.get_actor_label())
                            unreal.EditorLevelLibrary.destroy_actor(act)
                    response = {
                        "status": "success",
                        "result": {"deleted_count": len(deleted_names), "deleted": deleted_names[:100], "prefix": prefix}
                    }

            else:
                response = {"status": "error", "error": f"Unknown command: {cmd_type}"}

        except Exception as e:
            response = {"status": "error", "error": str(e), "traceback": traceback.format_exc()}

        _response_data[req_id] = response
        if req_id in _response_events:
            _response_events[req_id].set()

def listener_worker():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        server.bind(("127.0.0.1", PORT))
        server.listen(10)
        unreal.log(f"MCP Python Server listening on port {PORT}")
        while True:
            client, addr = server.accept()
            threading.Thread(target=handle_client, args=(client,), daemon=True).start()
    except Exception as e:
        unreal.log_error(f"MCP Python Server socket error: {e}")
    finally:
        server.close()

def handle_client(client):
    try:
        data = b""
        while True:
            chunk = client.recv(65536)
            if not chunk:
                break
            data += chunk
            try:
                msg = json.loads(data.decode("utf-8"))
                break
            except ValueError:
                continue

        if not data:
            client.close()
            return

        cmd_type = msg.get("type", "")
        params = msg.get("params", {})
        req_id = get_next_id()
        enqueued_time = time.time()
        event = threading.Event()
        _response_events[req_id] = event

        _request_queue.put((req_id, cmd_type, params, enqueued_time))

        if event.wait(timeout=30.0):
            resp = _response_data.pop(req_id, {"status": "error", "error": "No data returned"})
        else:
            resp = {"status": "error", "error": "Command execution timed out on Slate thread"}

        _response_events.pop(req_id, None)
        client.sendall(json.dumps(resp).encode("utf-8"))
    except Exception as e:
        err = {"status": "error", "error": str(e)}
        try:
            client.sendall(json.dumps(err).encode("utf-8"))
        except:
            pass
    finally:
        client.close()

def _tick_trampoline(delta_time):
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
    unreal.log_warning("MCP Unreal Engine Python Server initialized successfully!")
