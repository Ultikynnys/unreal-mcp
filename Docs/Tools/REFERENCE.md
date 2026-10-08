# Unreal MCP tool reference

Generated from the registered tools by `Python/scripts/gen_tool_docs.py`. Do not edit by hand: change the tool and re-run the script (`Python/scripts/test_docs_parity.py` fails when this file is stale).

78 tools. Parameters are the exact names the JSON schema accepts; `Sends` is the bridge command the tool issues, useful when correlating with editor logs. Replies are `{success, result, message}`: on failure the reason is in `error`/`message` and the structured detail stays in `result`.

## Assets: query, move/rename, delete, redirectors, import, graphs

| Tool | Parameters | Sends | Description |
| --- | --- | --- | --- |
| `delete_assets` | asset_paths, force | `delete_assets` | Delete assets and clear the redirector they leave behind. |
| `fixup_redirectors` | path, recursive, delete_redirectors, batch_size | `fixup_redirectors` | Fix up references to every redirector under a path and (optionally) delete them. |
| `get_asset_graph` | asset_path, direction, follow_redirectors | `get_asset_graph` | Get an asset's dependency/referencer packages (AssetRegistry, both directions). |
| `get_job_status` | job_id | `get_job_status` | Poll an asset job started by move_assets / move_folder / fixup_redirectors / resave_packages. |
| `list_redirectors` | path, recursive, resolve_destination | `list_redirectors` | List the ObjectRedirectors under a path - clutter left behind after assets move. |
| `move_assets` | moves, assets, destination_path, dry_run, fixup_redirectors | `move_assets` | Move/rename assets, saving and fixing source redirectors after each move. |
| `move_folder` | source_path, destination_path, recursive, dry_run | `move_folder` | Move folder contents preserving subfolders, fixing redirectors after each asset. |
| `resave_packages` | packages, path, recursive | `resave_packages` | Load and save packages. Runs asynchronously - returns a job_id; poll get_job_status. |

## Blueprint assets: create, components, properties, compile, spawn

| Tool | Parameters | Sends | Description |
| --- | --- | --- | --- |
| `add_component_to_blueprint` | blueprint_name, component_type, component_name, location, rotation, scale, component_properties | `add_component_to_blueprint` | Add a component to a Blueprint. |
| `compile_blueprint` | blueprint_name | `compile_blueprint` | Compile a Blueprint. |
| `create_blueprint` | name, parent_class | `create_blueprint` | Create a new Blueprint class. |
| `set_blueprint_property` | blueprint_name, property_name, property_value | `set_blueprint_property` | Set a property on a Blueprint class default object. |
| `set_component_property` | blueprint_name, component_name, property_name, property_value | `set_component_property` | Set a property on a component in a Blueprint. |
| `set_physics_properties` | blueprint_name, component_name, simulate_physics, gravity_enabled, mass, linear_damping, angular_damping | `set_physics_properties` | Set physics properties on a component. |
| `set_static_mesh_properties` | blueprint_name, component_name, static_mesh | `set_static_mesh_properties` | Set static mesh properties on a StaticMeshComponent. |

## Blueprint graph: nodes, pins, wiring, layout, validation

| Tool | Parameters | Sends | Description |
| --- | --- | --- | --- |
| `add_blueprint_event_node` | blueprint_name, event_name, node_position, graph_name | `add_blueprint_event_node` | Add an event node to a Blueprint's event graph. |
| `add_blueprint_function_node` | blueprint_name, target, function_name, params, node_position, graph_name | `add_blueprint_function_node` | Add a function call node to a Blueprint's graph. |
| `add_blueprint_get_self_component_reference` | blueprint_name, component_name, node_position, graph_name | `add_blueprint_get_self_component_reference` | Add a node that gets a reference to a component owned by the current Blueprint. |
| `add_blueprint_input_action_node` | blueprint_name, action_name, node_position, graph_name | `add_blueprint_input_action_node` | Add an input action event node to a Blueprint's event graph. |
| `add_blueprint_node` | blueprint_name, node_type, params, node_position, graph_name | `add_blueprint_node` | Add a control-flow / special node to a Blueprint's graph. |
| `add_blueprint_reroute_node` | blueprint_name, position, graph_name | `add_blueprint_reroute_node` | Add a reroute (knot) node to a Blueprint graph. |
| `add_blueprint_self_reference` | blueprint_name, node_position, graph_name | `add_blueprint_self_reference` | Add a 'Get Self' node to a Blueprint's graph that returns a reference to this actor. |
| `add_blueprint_variable` | blueprint_name, variable_name, variable_type, is_exposed, container | `add_blueprint_variable` | Add a variable to a Blueprint. |
| `apply_blueprint_plan` | blueprint_name, graph_name, plan_path, clear, auto_layout, async_, plan | `apply_blueprint_plan` | Build a whole Blueprint graph from a plan file in one call. |
| `auto_layout_blueprint_graph` | blueprint_name, graph_name, col_gap, row_gap, rebuild_routing, origin_x, origin_y | `auto_layout_blueprint_graph` | Auto-place a wired graph and rebuild its routing (layered layout). |
| `clear_blueprint_graph` | blueprint_name, graph_name, keep_entry_nodes | `clear_blueprint_graph` | Clear logic in a Blueprint graph (e.g. UserConstructionScript). |
| `connect_blueprint_nodes` | blueprint_name, source_node_id, source_pin, target_node_id, target_pin, graph_name, max_connection_length | `connect_blueprint_nodes` | Connect two nodes in a Blueprint's graph. |
| `delete_blueprint_node` | blueprint_name, node_id, graph_name | `delete_blueprint_node` | Delete a node from a Blueprint's graph. |
| `disconnect_blueprint_pin` | blueprint_name, node_id, pin_name, target_node_id, target_pin_name, graph_name | `disconnect_blueprint_pin` | Disconnect pin connections on a Blueprint node. |
| `find_blueprint_nodes` | blueprint_name, node_type, event_type, graph_name | `find_blueprint_nodes` | Find nodes in a Blueprint's graph. |
| `get_blueprint_graphs` | blueprint_name | `get_blueprint_graphs` | Get the list of all graphs in a Blueprint. |
| `get_blueprint_node_bounds` | blueprint_name, graph_name, include_nodes | `get_blueprint_node_bounds` | Get a graph's bounding box and (optionally) each node's position. |
| `get_plan_status` | job_id | `get_plan_status` | Poll an apply_blueprint_plan job. |
| `set_blueprint_node_pin_default` | blueprint_name, node_id, pin_name, value, graph_name | `set_blueprint_node_pin_default` | Set the literal/default value on an unconnected pin of a Blueprint node. |
| `set_blueprint_node_position` | blueprint_name, node_id, position, graph_name, max_connection_length, force | `set_blueprint_node_position` | Move an existing node in a Blueprint graph to a new position. |
| `validate_blueprint_graph` | blueprint_name, graph_name, max_connection_length | `validate_blueprint_graph` | Audit an existing Blueprint graph for the same design-rule breaks that node |

## Editor: actors, levels, viewport, captures, jobs, diagnostics

| Tool | Parameters | Sends | Description |
| --- | --- | --- | --- |
| `batch_execute` | actions, description, rollback_on_failure | `batch_execute` | Execute a list of operations in one batch. |
| `capture_pie_screenshot` | location, rotation, filename, width, height, fov | `capture_pie_screenshot` | Capture the Play-In-Editor (PIE) window from a specific world location and orientation. |
| `capture_viewport_screenshot` | filename | `capture_viewport_screenshot` | Capture the active editor viewport at its current resolution and return the saved path. |
| `console_command` | command | `console_command` | Run an allowlisted console command in the editor (stat/show/scalability/viewmodes). |
| `create_level` | map_path, template, overwrite | `create_level` | Create a new level asset in the content browser and switch to it. |
| `delete_actor` | name | `delete_actor` | Delete an actor by name. |
| `delete_actors_by_prefix` | prefix | `delete_actors_by_prefix` | Delete every actor in the level whose label starts with the given prefix. |
| `delete_level` | map_path, force | `delete_level` | Delete a level asset; refuses to delete the currently open level unless force=True. |
| `editor_play` | (none) | `editor_play` | Start Play-In-Editor on the current map (in-process). |
| `editor_stop` | (none) | `editor_stop` | Stop the running Play-In-Editor session. |
| `execute_python` | code | `execute_python` | Execute arbitrary Python code inside the running Unreal Editor on the main thread (hardened). |
| `get_actor_details` | name | `get_actor_details` | Inspect an actor by name, label, or path. Returns world bounds, components, materials, and light settings. |
| `get_actors_in_level` | class_filter, search, limit, offset | `get_actors_in_level` | Get a paginated and filterable list of actors in the current level. |
| `get_asset_details` | asset_path | `get_asset_details` | Get bounding box extents, dimensions, and material slots for a StaticMesh or asset. |
| `get_capabilities` | (none) | `get_capabilities` | Get server version, supported command types, and feature flags from Unreal Engine. |
| `get_current_level` | (none) | `get_current_level` | Get the currently open level: name, package path, and dirty state. |
| `get_import_status` | job_id | `get_import_status` | Poll an async import job started by import_asset. |
| `import_asset` | sources, destination_path, replace_existing, options | `import_asset` | Import external asset files (textures / meshes / audio) from disk into the project. |
| `list_levels` | (none) | `list_levels` | List all map (World) assets in /Game with their object paths. |
| `load_level` | map_path | `load_level` | Load a map asset and confirm the active editor world. |
| `query_assets` | path, recursive, asset_class, search, limit, offset | `query_assets` | Query asset paths with pagination, asset class filtering, and substring search. |
| `recover_editor` | (none) | `recover_editor` | Recover an editor stalled by the "restore unsaved files" crash-recovery prompt. |
| `reload_server` | (none) | `reload_server` | Hot-reload the Unreal-side server module in place (no editor restart needed). |
| `save_level` | destination_path, overwrite | `save_level` | Save current level, or save-as to destination_path with overwrite protection. |
| `set_actor_folder` | name, folder_path | `set_actor_folder` | Move an actor into a specified folder in the World Outliner. |
| `set_actor_material` | name, material_path, slot_index | `set_actor_material` | Assign a Material or Material Instance to an actor's StaticMeshComponent at a specific slot. |
| `set_actor_property` | name, property_name, property_value | `set_actor_property` | Set a property on an actor. |
| `set_actor_transform` | name, location, rotation, scale | `set_actor_transform` | Set the transform of an actor. |
| `set_viewport_camera` | location, rotation, game_view | `set_viewport_camera` | Set the Unreal Editor active viewport position, orientation, and game-view mode. |
| `spawn_actor` | name, type, location, rotation, allow_duplicate | `spawn_actor` | Create a new actor in the current level. |
| `spawn_blueprint_actor` | blueprint_name, actor_name, location, rotation | `spawn_blueprint_actor` | Spawn an actor from a Blueprint. |
| `spawn_instanced_mesh` | mesh_path, instances, name, folder_path | `spawn_instanced_mesh` | Spawn a single actor with a HierarchicalInstancedStaticMeshComponent containing multiple instance transforms. |
| `spawn_light_actor` | light_type, name, location, rotation, intensity, color, attenuation_radius, source_radius, mobility, folder_path | `spawn_light_actor` | Spawn a configured PointLight, SpotLight, RectLight, or DirectionalLight actor. |
| `spawn_mesh_actor` | mesh_path, name, location, rotation, scale, folder_path, allow_duplicate | `spawn_mesh_actor` | Spawn a StaticMeshActor with a specific static mesh asset into the current level. |
| `spawn_mesh_grid` | mesh_path, rows, cols, spacing_x, spacing_y, origin, rotation, scale, prefix, folder_path | `spawn_mesh_grid` | Spawn a 2D grid of StaticMeshActors atomically inside one transaction. |

## Project settings: input mappings

| Tool | Parameters | Sends | Description |
| --- | --- | --- | --- |
| `create_input_mapping` | action_name, key, input_type | `create_input_mapping` | Create an input mapping for the project. |

## UMG widgets: create widget blueprints and their components

| Tool | Parameters | Sends | Description |
| --- | --- | --- | --- |
| `add_button_to_widget` | widget_name, button_name, text, position, size, font_size, color, background_color | `add_button_to_widget` | Add a Button widget to a UMG Widget Blueprint. |
| `add_text_block_to_widget` | widget_name, text_block_name, text, position, size, font_size, color | `add_text_block_to_widget` | Add a Text Block widget to a UMG Widget Blueprint. |
| `add_widget_to_viewport` | widget_name, z_order | `add_widget_to_viewport` | Add a Widget Blueprint instance to the viewport. |
| `bind_widget_event` | widget_name, widget_component_name, event_name, function_name | `bind_widget_event` | Bind an event on a widget component to a function. |
| `create_umg_widget_blueprint` | widget_name, parent_class, path | `create_umg_widget_blueprint` | Create a new UMG Widget Blueprint. |
| `set_text_block_binding` | widget_name, text_block_name, binding_property, binding_type | `set_text_block_binding` | Set up a property binding for a Text Block widget. |
