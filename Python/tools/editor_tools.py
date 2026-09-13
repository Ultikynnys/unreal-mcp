"""
Editor Tools for Unreal MCP.

This module provides tools for controlling the Unreal Editor viewport and other editor functionality.
"""

import logging
from typing import Dict, List, Any, Optional
from mcp.server.fastmcp import FastMCP, Context

# Get logger
logger = logging.getLogger("UnrealMCP")

def register_editor_tools(mcp: FastMCP):
    """Register editor tools with the MCP server."""
    
    @mcp.tool()
    def get_actors_in_level(
        ctx: Context,
        class_filter: Optional[str] = None,
        search: Optional[str] = None,
        limit: int = 200,
        offset: int = 0
    ) -> Dict[str, Any]:
        """Get a paginated and filterable list of actors in the current level."""
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.warning("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            params = {
                "class_filter": class_filter,
                "search": search or "",
                "limit": limit,
                "offset": offset
            }
            response = unreal.send_command("get_actors_in_level", params)
            if not response:
                return {"success": False, "message": "No response from Unreal Engine"}
            return response
        except Exception as e:
            logger.error(f"Error getting actors: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def find_actors_by_name(ctx: Context, pattern: str) -> List[Dict[str, Any]]:
        """Find actors by name pattern."""
        from unreal_mcp_server import get_unreal_connection
        
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.warning("Failed to connect to Unreal Engine")
                return []
                
            response = unreal.send_command("find_actors_by_name", {
                "pattern": pattern
            })
            
            if not response:
                return []
                
            # Backend replies are normalized to {"success", "result", "message"};
            # the actor array lives under result.actors, not at the top level.
            result = response.get("result") or {}
            return result.get("actors", [])
            
        except Exception as e:
            logger.error(f"Error finding actors: {e}")
            return []
    
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
            
        Returns:
            Dict containing the created actor's properties
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            # Ensure all parameters are properly formatted
            params = {
                "name": name,
                "type": type.upper(),  # Make sure type is uppercase
                "location": location,
                "rotation": rotation,
                "allow_duplicate": allow_duplicate
            }
            
            # Validate location and rotation formats
            for param_name in ["location", "rotation"]:
                param_value = params[param_name]
                if not isinstance(param_value, list) or len(param_value) != 3:
                    logger.error(f"Invalid {param_name} format: {param_value}. Must be a list of 3 float values.")
                    return {"success": False, "message": f"Invalid {param_name} format. Must be a list of 3 float values."}
                # Ensure all values are float
                params[param_name] = [float(val) for val in param_value]
            
            logger.info(f"Creating actor '{name}' of type '{type}' with params: {params}")
            response = unreal.send_command("spawn_actor", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            # Log the complete response for debugging
            logger.info(f"Actor creation response: {response}")
            
            # Handle error responses correctly
            if response.get("success") is False:
                error_message = response.get("message", "Unknown error")
                logger.error(f"Error creating actor: {error_message}")
                return {"success": False, "message": error_message}
            
            return response
            
        except Exception as e:
            error_msg = f"Error creating actor: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
    
    @mcp.tool()
    def delete_actor(ctx: Context, name: str) -> Dict[str, Any]:
        """Delete an actor by name."""
        from unreal_mcp_server import get_unreal_connection
        
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
                
            response = unreal.send_command("delete_actor", {
                "name": name
            })
            return response or {}
            
        except Exception as e:
            logger.error(f"Error deleting actor: {e}")
            return {}
    
    @mcp.tool()
    def set_actor_transform(
        ctx: Context,
        name: str,
        location: List[float]  = None,
        rotation: List[float]  = None,
        scale: List[float] = None
    ) -> Dict[str, Any]:
        """Set the transform of an actor."""
        from unreal_mcp_server import get_unreal_connection
        
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
                
            params = {"name": name}
            if location is not None:
                params["location"] = location
            if rotation is not None:
                params["rotation"] = rotation
            if scale is not None:
                params["scale"] = scale
                
            response = unreal.send_command("set_actor_transform", params)
            return response or {}
            
        except Exception as e:
            logger.error(f"Error setting transform: {e}")
            return {}
    
    @mcp.tool()
    def get_actor_properties(ctx: Context, name: str) -> Dict[str, Any]:
        """Get all properties of an actor (redirects to get_actor_details)."""
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            return unreal.send_command("get_actor_details", {"name": name})
        except Exception as e:
            logger.error(f"Error getting actor properties: {e}")
            return {"success": False, "message": str(e)}

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
        from unreal_mcp_server import get_unreal_connection
        
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
                
            response = unreal.send_command("set_actor_property", {
                "name": name,
                "property_name": property_name,
                "property_value": property_value
            })
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Set actor property response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error setting actor property: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    # @mcp.tool() commented out because it's buggy
    def focus_viewport(
        ctx: Context,
        target: str = None,
        location: List[float] = None,
        distance: float = 1000.0,
        orientation: List[float] = None
    ) -> Dict[str, Any]:
        """
        Focus the viewport on a specific actor or location.
        
        Args:
            target: Name of the actor to focus on (if provided, location is ignored)
            location: [X, Y, Z] coordinates to focus on (used if target is None)
            distance: Distance from the target/location
            orientation: Optional [Pitch, Yaw, Roll] for the viewport camera
            
        Returns:
            Response from Unreal Engine
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
                
            params = {}
            if target:
                params["target"] = target
            elif location:
                params["location"] = location
            
            if distance:
                params["distance"] = distance
                
            if orientation:
                params["orientation"] = orientation
                
            response = unreal.send_command("focus_viewport", params)
            return response or {}
            
        except Exception as e:
            logger.error(f"Error focusing viewport: {e}")
            return {"success": False, "message": str(e)}

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
        from unreal_mcp_server import get_unreal_connection
        
        try:
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            # Ensure all parameters are properly formatted
            params = {
                "blueprint_name": blueprint_name,
                "actor_name": actor_name,
                "location": location or [0.0, 0.0, 0.0],
                "rotation": rotation or [0.0, 0.0, 0.0]
            }
            
            # Validate location and rotation formats
            for param_name in ["location", "rotation"]:
                param_value = params[param_name]
                if not isinstance(param_value, list) or len(param_value) != 3:
                    logger.error(f"Invalid {param_name} format: {param_value}. Must be a list of 3 float values.")
                    return {"success": False, "message": f"Invalid {param_name} format. Must be a list of 3 float values."}
                # Ensure all values are float
                params[param_name] = [float(val) for val in param_value]
            
            logger.info(f"Spawning blueprint actor with params: {params}")
            response = unreal.send_command("spawn_blueprint_actor", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Spawn blueprint actor response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error spawning blueprint actor: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def get_capabilities(ctx: Context) -> Dict[str, Any]:
        """Get server version, supported command types, and feature flags from Unreal Engine."""
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            return unreal.send_command("get_capabilities", {})
        except Exception as e:
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def query_assets(
        ctx: Context,
        path: str = "/Game",
        recursive: bool = True,
        asset_class: Optional[str] = None,
        search: Optional[str] = None,
        limit: int = 100,
        offset: int = 0
    ) -> Dict[str, Any]:
        """Query asset paths with pagination, asset class filtering, and substring search."""
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            params = {
                "path": path,
                "recursive": recursive,
                "asset_class": asset_class,
                "search": search or "",
                "limit": limit,
                "offset": offset
            }
            return unreal.send_command("query_assets", params)
        except Exception as e:
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def get_asset_details(ctx: Context, asset_path: str) -> Dict[str, Any]:
        """Get bounding box extents, dimensions, and material slots for a StaticMesh or asset."""
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            return unreal.send_command("get_asset_details", {"asset_path": asset_path})
        except Exception as e:
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def get_actor_details(ctx: Context, name: str) -> Dict[str, Any]:
        """Inspect an actor by name, label, or path. Returns world bounds, components, materials, and light settings."""
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            return unreal.send_command("get_actor_details", {"name": name})
        except Exception as e:
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def spawn_mesh_actor(
        ctx: Context,
        mesh_path: str,
        name: str = "MeshActor",
        location: Optional[List[float]] = None,
        rotation: Optional[List[float]] = None,
        scale: Optional[List[float]] = None,
        folder_path: Optional[str] = None,
        allow_duplicate: bool = False
    ) -> Dict[str, Any]:
        """Spawn a StaticMeshActor with a specific static mesh asset into the current level."""
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            params = {
                "mesh_path": mesh_path,
                "name": name,
                "location": location or [0.0, 0.0, 0.0],
                "rotation": rotation or [0.0, 0.0, 0.0],
                "scale": scale or [1.0, 1.0, 1.0],
                "folder_path": folder_path,
                "allow_duplicate": allow_duplicate
            }
            return unreal.send_command("spawn_mesh_actor", params)
        except Exception as e:
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def batch_execute(
        ctx: Context,
        actions: List[Dict[str, Any]],
        description: str = "MCP Batch Operation",
        rollback_on_failure: bool = True
    ) -> Dict[str, Any]:
        """Execute a list of atomic operations inside a single ScopedEditorTransaction with rollback."""
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            params = {
                "actions": actions,
                "description": description,
                "rollback_on_failure": rollback_on_failure
            }
            return unreal.send_command("batch_execute", params)
        except Exception as e:
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def set_viewport_camera(
        ctx: Context,
        location: List[float],
        rotation: List[float],
        game_view: Optional[bool] = None
    ) -> Dict[str, Any]:
        """Set the Unreal Editor active viewport position, orientation, and game-view mode."""
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            params = {
                "location": location,
                "rotation": rotation,
                "game_view": game_view
            }
            return unreal.send_command("set_viewport_camera", params)
        except Exception as e:
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def capture_viewport_screenshot(
        ctx: Context,
        filename: str = "MCP_Screenshot.png",
        width: int = 1440,
        height: int = 1000
    ) -> Dict[str, Any]:
        """Trigger an automation high-resolution screenshot and return the filesystem destination path."""
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            params = {
                "filename": filename,
                "width": width,
                "height": height
            }
            return unreal.send_command("capture_viewport_screenshot", params)
        except Exception as e:
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def save_level(
        ctx: Context,
        destination_path: Optional[str] = None,
        overwrite: bool = False
    ) -> Dict[str, Any]:
        """Save current level, or save-as to destination_path with overwrite protection."""
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            params = {
                "destination_path": destination_path,
                "overwrite": overwrite
            }
            return unreal.send_command("save_level", params)
        except Exception as e:
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def load_level(ctx: Context, map_path: str) -> Dict[str, Any]:
        """Load a map asset and confirm the active editor world."""
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            return unreal.send_command("load_level", {"map_path": map_path})
        except Exception as e:
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def execute_python(ctx: Context, code: str) -> Dict[str, Any]:
        """Execute arbitrary Python code inside the running Unreal Editor on the main thread (hardened)."""
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            return unreal.send_command("execute_python", {"code": code})
        except Exception as e:
            return {"success": False, "message": str(e)}


    @mcp.tool()
    def create_level(
        ctx: Context,
        map_path: str,
        template: str = "empty",
        overwrite: bool = False
    ) -> Dict[str, Any]:
        """Create a new level asset in the content browser and switch to it."""
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            params = {
                "map_path": map_path,
                "template": template,
                "overwrite": overwrite
            }
            return unreal.send_command("create_level", params)
        except Exception as e:
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def spawn_mesh_grid(
        ctx: Context,
        mesh_path: str,
        rows: int = 1,
        cols: int = 1,
        spacing_x: float = 200.0,
        spacing_y: float = 200.0,
        origin: Optional[List[float]] = None,
        rotation: Optional[List[float]] = None,
        scale: Optional[List[float]] = None,
        prefix: str = "GridActor",
        folder_path: str = "Environment/Grids"
    ) -> Dict[str, Any]:
        """Spawn a 2D grid of StaticMeshActors atomically inside one transaction."""
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
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
            return unreal.send_command("spawn_mesh_grid", params)
        except Exception as e:
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def spawn_instanced_mesh(
        ctx: Context,
        mesh_path: str,
        instances: List[Dict[str, Any]],
        name: str = "InstancedActor",
        folder_path: str = "Environment/Instances"
    ) -> Dict[str, Any]:
        """Spawn a single actor with a HierarchicalInstancedStaticMeshComponent containing multiple instance transforms."""
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            params = {
                "mesh_path": mesh_path,
                "instances": instances,
                "name": name,
                "folder_path": folder_path
            }
            return unreal.send_command("spawn_instanced_mesh", params)
        except Exception as e:
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def spawn_light_actor(
        ctx: Context,
        light_type: str = "PointLight",
        name: str = "LightActor",
        location: Optional[List[float]] = None,
        rotation: Optional[List[float]] = None,
        intensity: float = 3000.0,
        color: Optional[List[float]] = None,
        attenuation_radius: float = 1000.0,
        source_radius: float = 20.0,
        mobility: str = "movable",
        folder_path: str = "Environment/Lighting"
    ) -> Dict[str, Any]:
        """Spawn a configured PointLight, SpotLight, RectLight, or DirectionalLight actor."""
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
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
            return unreal.send_command("spawn_light_actor", params)
        except Exception as e:
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def set_actor_folder(
        ctx: Context,
        name: str,
        folder_path: str
    ) -> Dict[str, Any]:
        """Move an actor into a specified folder in the World Outliner."""
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            return unreal.send_command("set_actor_folder", {"name": name, "folder_path": folder_path})
        except Exception as e:
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def set_actor_material(
        ctx: Context,
        name: str,
        material_path: str,
        slot_index: int = 0
    ) -> Dict[str, Any]:
        """Assign a Material or Material Instance to an actor's StaticMeshComponent at a specific slot."""
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            params = {
                "name": name,
                "material_path": material_path,
                "slot_index": slot_index
            }
            return unreal.send_command("set_actor_material", params)
        except Exception as e:
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def reload_server(ctx: Context) -> Dict[str, Any]:
        """Hot-reload the Unreal-side server module in place (no editor restart needed)."""
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            return unreal.send_command("reload_server", {})
        except Exception as e:
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def delete_level(ctx: Context, map_path: str, force: bool = False) -> Dict[str, Any]:
        """Delete a level asset; refuses to delete the currently open level unless force=True."""
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            return unreal.send_command("delete_level", {"map_path": map_path, "force": force})
        except Exception as e:
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def delete_actors_by_prefix(ctx: Context, prefix: str) -> Dict[str, Any]:
        """Delete every actor in the level whose label starts with the given prefix."""
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            return unreal.send_command("delete_actors_by_prefix", {"prefix": prefix})
        except Exception as e:
            return {"success": False, "message": str(e)}

    logger.info("Editor tools registered successfully")
