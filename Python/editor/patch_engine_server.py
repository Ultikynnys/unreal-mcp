"""
Append the 6 new advanced endpoints into mcp_unreal_engine.py cleanly.
"""
from pathlib import Path

target_files = [
    Path('Python/mcp_unreal_engine.py'),
    Path('C:/Users/uraan/.reasonix/tools/unreal-mcp/Python/scripts/mcp_unreal_engine.py')
]

additions_code = '''
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
                    new_world = unreal.EditorLevelLibrary.new_level(map_path)
                    response = {
                        "status": "success" if new_world else "error",
                        "result": {
                            "created": new_world is not None,
                            "map_path": map_path,
                            "active_world": new_world.get_path_name() if new_world else None
                        }
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

            # 12. Instanced Mesh Spawning (HISM - 1 actor for thousands of instances)
            elif cmd_type == "spawn_instanced_mesh":
                mesh_path = params.get("mesh_path", "")
                actor_name = params.get("name", "InstancedActor")
                folder = params.get("folder_path", "Environment/Instances")
                instances = params.get("instances", [])  # list of {location: [x,y,z], rotation: [p,y,r], scale: [x,y,z]}

                mesh_asset = unreal.EditorAssetLibrary.load_asset(mesh_path)
                if not mesh_asset or not isinstance(mesh_asset, unreal.StaticMesh):
                    response = {"status": "error", "error": f"Invalid StaticMesh asset: {mesh_path}"}
                else:
                    with unreal.ScopedEditorTransaction(f"Spawn Instanced Mesh {actor_name}"):
                        actor = unreal.EditorLevelLibrary.spawn_actor_from_class(
                            unreal.StaticMeshActor,
                            unreal.Vector(0, 0, 0),
                            unreal.Rotator(0, 0, 0)
                        )
                        actor.set_actor_label(actor_name)
                        if folder:
                            actor.set_folder_path(folder)

                        # Replace standard component with HierarchicalInstancedStaticMeshComponent
                        hism = unreal.HierarchicalInstancedStaticMeshComponent(actor)
                        hism.set_static_mesh(mesh_asset)
                        actor.add_instance_component(hism)

                        added_count = 0
                        for inst in instances:
                            loc = inst.get("location", [0, 0, 0])
                            r = inst.get("rotation", [0, 0, 0])
                            s = inst.get("scale", [1, 1, 1])
                            t = unreal.Transform(
                                location=unreal.Vector(loc[0], loc[1], loc[2]),
                                rotation=unreal.Rotator(pitch=r[0], yaw=r[1], roll=r[2]),
                                scale=unreal.Vector(s[0], s[1], s[2])
                            )
                            hism.add_instance(t)
                            added_count += 1

                    response = {
                        "status": "success",
                        "result": {
                            "actor": actor.get_actor_label(),
                            "mesh": mesh_path,
                            "instance_count": added_count,
                            "folder": folder
                        }
                    }

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

                cls_map = {
                    "pointlight": unreal.PointLight,
                    "spotlight": unreal.SpotLight,
                    "rectlight": unreal.RectLight,
                    "directionallight": unreal.DirectionalLight
                }
                cls = cls_map.get(light_type, unreal.PointLight)

                u_loc = unreal.Vector(loc[0], loc[1], loc[2])
                u_rot = unreal.Rotator(pitch=rot[0], yaw=rot[1], roll=rot[2])

                actor = unreal.EditorLevelLibrary.spawn_actor_from_class(cls, u_loc, u_rot)
                if not actor:
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
                            except:
                                pass

                    response = {
                        "status": "success",
                        "result": inspect_actor_data(actor)
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
'''

for fpath in target_files:
    text = fpath.read_text(encoding='utf-8')
    
    # 1. Update PROTOCOL_VERSION
    text = text.replace('PROTOCOL_VERSION = "1.1.0"', 'PROTOCOL_VERSION = "1.2.0"')

    # 2. Add folder_path to spawn_mesh_actor
    old_spawn_mesh = 'folder = params.get("folder_path", "")'
    if old_spawn_mesh not in text:
        text = text.replace(
            'actor.set_actor_label(actor_name)',
            'actor.set_actor_label(actor_name)\n                        folder_param = params.get("folder_path")\n                        if folder_param:\n                            actor.set_folder_path(folder_param)'
        )

    # 3. Add folder_path to spawn_actor
    text = text.replace(
        'actor.set_actor_label(name)\n                        actor.set_actor_scale3d(u_scale)',
        'actor.set_actor_label(name)\n                        folder_param = params.get("folder_path")\n                        if folder_param:\n                            actor.set_folder_path(folder_param)\n                        actor.set_actor_scale3d(u_scale)'
    )

    # 4. Insert new command handlers before the final `else:`
    marker = '            else:\n                response = {"status": "error", "error": f"Unknown command: {cmd_type}"}'
    assert marker in text, f'Marker not found in {fpath}'
    text = text.replace(marker, additions_code + '\n' + marker)

    fpath.write_text(text, encoding='utf-8')
    print(f'Successfully patched {fpath}')
