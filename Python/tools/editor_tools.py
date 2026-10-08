"""
Editor Tools for Unreal MCP.

This module provides tools for controlling the Unreal Editor viewport and other editor functionality.
"""

import logging
from typing import Dict, List, Any, Optional
from mcp.server.fastmcp import FastMCP, Context
from tools.mcp_client import call_unreal, require_vectors3

# Get logger
logger = logging.getLogger("UnrealMCP")

def register_editor_tools(mcp: FastMCP):
    """Register editor tools with the MCP server."""
    
    @mcp.tool()
    def get_actors_in_level(
        ctx: Context,
        class_filter: str = None,
        search: str = None,
        limit: int = 200,
        offset: int = 0
    ) -> Dict[str, Any]:
        """Get a paginated and filterable list of actors in the current level."""
        params = {
            "class_filter": class_filter,
            "search": search or "",
            "limit": limit,
            "offset": offset
        }
        return call_unreal("get_actors_in_level", params)

    @mcp.tool()
    def spawn_actor(
        ctx: Context,
        name: str,
        type: str,
        location: List[float] = [0.0, 0.0, 0.0],
        rotation: List[float] = [0.0, 0.0, 0.0],
        allow_duplicate: bool = False
    ) -> Dict[str, Any]:
        """Create a new actor in the current level.
        
        Args:
            ctx: The MCP context
            name: The name to give the new actor (must be unique)
            type: The type of actor to create (e.g. StaticMeshActor, PointLight)
            location: The [x, y, z] world location to spawn at
            rotation: The [pitch, yaw, roll] rotation in degrees
            allow_duplicate: Permit a duplicate base name; a free suffixed name is derived
            
        Returns:
            Dict containing the created actor's properties
        """
        # Send the type verbatim; the C++ handler matches it case-insensitively.
        params = {
            "name": name,
            "type": type,
            "location": location,
            "rotation": rotation,
            "allow_duplicate": allow_duplicate
        }
        error = require_vectors3(params, "location", "rotation")
        if error:
            return {"success": False, "message": error}
        return call_unreal("spawn_actor", params)
    
    @mcp.tool()
    def delete_actor(ctx: Context, name: str) -> Dict[str, Any]:
        """Delete an actor by name."""
        
        return call_unreal("delete_actor", {
                "name": name
            })
    
    @mcp.tool()
    def set_actor_transform(
        ctx: Context,
        name: str,
        location: List[float]  = None,
        rotation: List[float]  = None,
        scale: List[float] = None
    ) -> Dict[str, Any]:
        """Set the transform of an actor."""
        
        params = {"name": name}
        if location is not None:
            params["location"] = location
        if rotation is not None:
            params["rotation"] = rotation
        if scale is not None:
            params["scale"] = scale
        return call_unreal("set_actor_transform", params)
    
    @mcp.tool()
    def set_actor_property(
        ctx: Context,
        name: str,
        property_name: str,
        property_value,
    ) -> Dict[str, Any]:
        """
        Set a property on an actor.
        
        Args:
            name: Name of the actor
            property_name: Name of the property to set
            property_value: Value to set the property to
            
        Returns:
            Dict containing response from Unreal with operation status
        """
        
        return call_unreal("set_actor_property", {
                "name": name,
                "property_name": property_name,
                "property_value": property_value
            })

    @mcp.tool()
    def spawn_blueprint_actor(
        ctx: Context,
        blueprint_name: str,
        actor_name: str,
        location: List[float] = [0.0, 0.0, 0.0],
        rotation: List[float] = [0.0, 0.0, 0.0]
    ) -> Dict[str, Any]:
        """Spawn an actor from a Blueprint.
        
        Args:
            ctx: The MCP context
            blueprint_name: Name of the Blueprint to spawn from
            actor_name: Name to give the spawned actor
            location: The [x, y, z] world location to spawn at
            rotation: The [pitch, yaw, roll] rotation in degrees
            
        Returns:
            Dict containing the spawned actor's properties
        """
        params = {
            "blueprint_name": blueprint_name,
            "actor_name": actor_name,
            "location": location or [0.0, 0.0, 0.0],
            "rotation": rotation or [0.0, 0.0, 0.0]
        }
        error = require_vectors3(params, "location", "rotation")
        if error:
            return {"success": False, "message": error}
        return call_unreal("spawn_blueprint_actor", params)

    @mcp.tool()
    def get_capabilities(ctx: Context) -> Dict[str, Any]:
        """Get server version, supported command types, and feature flags from Unreal Engine."""
        return call_unreal("get_capabilities", {})

    @mcp.tool()
    def query_assets(
        ctx: Context,
        path: str = "/Game",
        recursive: bool = True,
        asset_class: str = None,
        search: str = None,
        limit: int = 100,
        offset: int = 0
    ) -> Dict[str, Any]:
        """Query asset paths with pagination, asset class filtering, and substring search."""
        params = {
            "path": path,
            "recursive": recursive,
            "asset_class": asset_class,
            "search": search or "",
            "limit": limit,
            "offset": offset
        }
        return call_unreal("query_assets", params)

    @mcp.tool()
    def get_asset_details(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """Get bounding box extents, dimensions, and material slots for a StaticMesh or asset."""
        return call_unreal("get_asset_details", {"asset_path": asset_path})

    @mcp.tool()
    def get_actor_details(ctx: Context, name: str) -> Dict[str, Any]:
        """Inspect an actor by name, label, or path. Returns world bounds, components, materials, and light settings."""
        return call_unreal("get_actor_details", {"name": name})

    @mcp.tool()
    def spawn_mesh_actor(
        ctx: Context,
        mesh_path: str,
        name: str = "MeshActor",
        location: List[float] = None,
        rotation: List[float] = None,
        scale: List[float] = None,
        folder_path: str = None,
        allow_duplicate: bool = False
    ) -> Dict[str, Any]:
        """Spawn a StaticMeshActor with a specific static mesh asset into the current level."""
        params = {
            "mesh_path": mesh_path,
            "name": name,
            "location": location or [0.0, 0.0, 0.0],
            "rotation": rotation or [0.0, 0.0, 0.0],
            "scale": scale or [1.0, 1.0, 1.0],
            "folder_path": folder_path,
            "allow_duplicate": allow_duplicate
        }
        return call_unreal("spawn_mesh_actor", params)

    @mcp.tool()
    def batch_execute(
        ctx: Context,
        actions: List[Dict[str, Any]],
        description: str = "MCP Batch Operation",
        rollback_on_failure: bool = True
    ) -> Dict[str, Any]:
        """Execute a list of operations in one batch.

        Each sub-command is routed through the full command surface (editor, blueprint,
        blueprint-node, project, UMG). When rollback_on_failure is set and any action
        fails, the whole batch is reverted: every actor and component is snapshotted
        before the batch and restored after (the editor undo stack does not revert
        changes made over the socket bridge), and actors spawned during the batch are
        destroyed. Returns { results, count, failures, rolled_back }.
        """
        params = {
            "actions": actions,
            "description": description,
            "rollback_on_failure": rollback_on_failure
        }
        return call_unreal("batch_execute", params)

    @mcp.tool()
    def set_viewport_camera(
        ctx: Context,
        location: List[float],
        rotation: List[float],
        game_view: bool = None
    ) -> Dict[str, Any]:
        """Set the Unreal Editor active viewport position, orientation, and game-view mode."""
        params = {
            "location": location,
            "rotation": rotation,
            "game_view": game_view
        }
        return call_unreal("set_viewport_camera", params)

    @mcp.tool()
    def capture_viewport_screenshot(
        ctx: Context,
        filename: str = "MCP_Screenshot.png"
    ) -> Dict[str, Any]:
        """Capture the active editor viewport at its current resolution and return the saved path.

        The image matches the live viewport size (the backend reads the viewport framebuffer),
        so there is no width/height to set. Use capture_pie_screenshot's width/height to size a
        PIE render.
        """
        params = {
            "filename": filename
        }
        return call_unreal("capture_viewport_screenshot", params)

    @mcp.tool()
    def capture_pie_screenshot(
        ctx: Context,
        location: List[float] = None,
        rotation: List[float] = None,
        filename: str = "MCP_PIE_Screenshot.png",
        width: int = 1280,
        height: int = 720,
        fov: float = None
    ) -> Dict[str, Any]:
        """Capture the Play-In-Editor (PIE) window from a specific world location and orientation.

        Renders the running PIE world from a transient camera placed at location/rotation (or the
        PIE player's current view if omitted) and saves a PNG under Saved/Screenshots. Requires an
        active PIE session. Renders the 3D scene only (no UMG/HUD overlay).

        Args:
            location: Optional [X, Y, Z] world location for the capture camera
            rotation: Optional [Pitch, Yaw, Roll] for the capture camera
            filename: Output PNG name (relative -> Saved/Screenshots/)
            width: Image width in pixels (clamped 16..4096, default 1280)
            height: Image height in pixels (clamped 16..4096, default 720)
            fov: Optional horizontal field of view in degrees
        """
        params = {
            "filename": filename,
            "width": width,
            "height": height
        }
        if location is not None:
            params["location"] = location
        if rotation is not None:
            params["rotation"] = rotation
        if fov is not None:
            params["fov"] = fov
        return call_unreal("capture_pie_screenshot", params)

    @mcp.tool()
    def save_level(
        ctx: Context,
        destination_path: str = None,
        overwrite: bool = False
    ) -> Dict[str, Any]:
        """Save current level, or save-as to destination_path with overwrite protection."""
        params = {
            "destination_path": destination_path,
            "overwrite": overwrite
        }
        return call_unreal("save_level", params)

    @mcp.tool()
    def load_level(ctx: Context, map_path: str) -> Dict[str, Any]:
        """Load a map asset and confirm the active editor world."""
        return call_unreal("load_level", {"map_path": map_path})

    @mcp.tool()
    def execute_python(ctx: Context, code: str) -> Dict[str, Any]:
        """Execute arbitrary Python code inside the running Unreal Editor on the main thread (hardened)."""
        return call_unreal("execute_python", {"code": code})


    @mcp.tool()
    def create_level(
        ctx: Context,
        map_path: str,
        template: str = "empty",
        overwrite: bool = False
    ) -> Dict[str, Any]:
        """Create a new level asset in the content browser and switch to it."""
        params = {
            "map_path": map_path,
            "template": template,
            "overwrite": overwrite
        }
        return call_unreal("create_level", params)

    @mcp.tool()
    def spawn_mesh_grid(
        ctx: Context,
        mesh_path: str,
        rows: int = 1,
        cols: int = 1,
        spacing_x: float = 200.0,
        spacing_y: float = 200.0,
        origin: List[float] = None,
        rotation: List[float] = None,
        scale: List[float] = None,
        prefix: str = "GridActor",
        folder_path: str = "Environment/Grids"
    ) -> Dict[str, Any]:
        """Spawn a 2D grid of StaticMeshActors atomically inside one transaction."""
        params = {
            "mesh_path": mesh_path,
            "rows": rows,
            "cols": cols,
            "spacing_x": spacing_x,
            "spacing_y": spacing_y,
            "origin": origin or [0.0, 0.0, 0.0],
            "rotation": rotation or [0.0, 0.0, 0.0],
            "scale": scale or [1.0, 1.0, 1.0],
            "prefix": prefix,
            "folder_path": folder_path
        }
        return call_unreal("spawn_mesh_grid", params)

    @mcp.tool()
    def spawn_instanced_mesh(
        ctx: Context,
        mesh_path: str,
        instances: List[Dict[str, Any]],
        name: str = "InstancedActor",
        folder_path: str = "Environment/Instances"
    ) -> Dict[str, Any]:
        """Spawn a single actor with a HierarchicalInstancedStaticMeshComponent containing multiple instance transforms."""
        params = {
            "mesh_path": mesh_path,
            "instances": instances,
            "name": name,
            "folder_path": folder_path
        }
        return call_unreal("spawn_instanced_mesh", params)

    @mcp.tool()
    def spawn_light_actor(
        ctx: Context,
        light_type: str = "PointLight",
        name: str = "LightActor",
        location: List[float] = None,
        rotation: List[float] = None,
        intensity: float = 3000.0,
        color: List[float] = None,
        attenuation_radius: float = 1000.0,
        source_radius: float = 20.0,
        mobility: str = "movable",
        folder_path: str = "Environment/Lighting"
    ) -> Dict[str, Any]:
        """Spawn a configured PointLight, SpotLight, RectLight, or DirectionalLight actor."""
        params = {
            "light_type": light_type,
            "name": name,
            "location": location or [0.0, 0.0, 0.0],
            "rotation": rotation or [0.0, 0.0, 0.0],
            "intensity": intensity,
            "color": color or [1.0, 1.0, 1.0],
            "attenuation_radius": attenuation_radius,
            "source_radius": source_radius,
            "mobility": mobility,
            "folder_path": folder_path
        }
        return call_unreal("spawn_light_actor", params)

    @mcp.tool()
    def set_actor_folder(
        ctx: Context,
        name: str,
        folder_path: str
    ) -> Dict[str, Any]:
        """Move an actor into a specified folder in the World Outliner."""
        return call_unreal("set_actor_folder", {"name": name, "folder_path": folder_path})

    @mcp.tool()
    def set_actor_material(
        ctx: Context,
        name: str,
        material_path: str,
        slot_index: int = 0
    ) -> Dict[str, Any]:
        """Assign a Material or Material Instance to an actor's StaticMeshComponent at a specific slot."""
        params = {
            "name": name,
            "material_path": material_path,
            "slot_index": slot_index
        }
        return call_unreal("set_actor_material", params)

    @mcp.tool()
    def reload_server(ctx: Context) -> Dict[str, Any]:
        """Hot-reload the Unreal-side server module in place (no editor restart needed)."""
        return call_unreal("reload_server", {})

    @mcp.tool()
    def delete_level(ctx: Context, map_path: str, force: bool = False) -> Dict[str, Any]:
        """Delete a level asset; refuses to delete the currently open level unless force=True."""
        return call_unreal("delete_level", {"map_path": map_path, "force": force})

    @mcp.tool()
    def delete_actors_by_prefix(ctx: Context, prefix: str) -> Dict[str, Any]:
        """Delete every actor in the level whose label starts with the given prefix."""
        return call_unreal("delete_actors_by_prefix", {"prefix": prefix})

    @mcp.tool()
    def import_asset(
        ctx: Context,
        sources: List[str],
        destination_path: str = "/Game",
        replace_existing: bool = True,
        options: Dict[str, Any] = None
    ) -> Dict[str, Any]:
        """Import external asset files (textures / meshes / audio) from disk into the project.

        Runs ASYNCHRONOUSLY on the Unreal side: this returns a job_id immediately. Poll
        get_import_status(job_id) until state is "done" or "failed". The import is deferred to
        the next editor tick so it never crashes the editor (unlike importing inline).

        Format is auto-detected: FBX/OBJ/glTF/USD meshes, PNG/JPG/EXR/TGA/HDR textures,
        WAV/OGG audio.

        options (textures): {"srgb": bool, "is_normal_map": bool,
                             "compression": "default" | "grayscale" | "normalmap"}
        """
        params = {
            "sources": sources,
            "destination_path": destination_path,
            "replace_existing": replace_existing,
            "options": options or {}
        }
        return call_unreal("import_asset", params)

    @mcp.tool()
    def get_import_status(ctx: Context, job_id: str) -> Dict[str, Any]:
        """Poll an async import job started by import_asset.

        state is one of queued | running | done | failed. On success, assets lists the
        imported object paths; log carries per-file notes; error is set on failure.
        """
        return call_unreal("get_import_status", {"job_id": job_id})

    @mcp.tool()
    def recover_editor(ctx: Context) -> Dict[str, Any]:
        """Recover an editor stalled by the "restore unsaved files" crash-recovery prompt.

        After an abnormal shutdown Unreal writes Saved/Autosaves/PackageRestoreData.json and
        on the next launch shows a modal "restore unsaved files" dialog that blocks the core
        ticker (so plan / layout jobs never run). This clears that on-disk state and dismisses
        the modal so the editor resumes. Call it right after the bridge connects.
        """
        return call_unreal("recover_editor", {})

    @mcp.tool()
    def console_command(ctx: Context, command: str) -> Dict[str, Any]:
        """Run an allowlisted console command in the editor (stat/show/scalability/viewmodes).

        Allowed: stat *, show *, r.*, sg.*, foliage.*, grass.*, t.MaxFPS, HighResShot.
        Mutating commands (exec, quit, gunit, Log off) are refused. Output goes to the
        editor log; the reply reports whether the editor marked the command handled.
        """
        return call_unreal("console_command", {"command": command})

    @mcp.tool()
    def editor_play(ctx: Context) -> Dict[str, Any]:
        """Start Play-In-Editor on the current map (in-process).

        Returns state 'requested' (PIE starts on the next editor tick), 'already_playing',
        or an error. Pair with capture_pie_screenshot and editor_stop for an automated
        verify loop. PIE start can take a few seconds; poll editor_play again or check
        get_current_level afterwards.
        """
        return call_unreal("editor_play", {})

    @mcp.tool()
    def editor_stop(ctx: Context) -> Dict[str, Any]:
        """Stop the running Play-In-Editor session.

        Returns state 'stopping' (PIE ends on the next editor tick) or 'not_playing'.
        """
        return call_unreal("editor_stop", {})

    @mcp.tool()
    def list_levels(ctx: Context) -> Dict[str, Any]:
        """List all map (World) assets in /Game with their object paths."""
        return call_unreal("list_levels", {})

    @mcp.tool()
    def get_current_level(ctx: Context) -> Dict[str, Any]:
        """Get the currently open level: name, package path, and dirty state."""
        return call_unreal("get_current_level", {})

    logger.info("Editor tools registered successfully")
