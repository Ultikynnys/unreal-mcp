"""
Extend editor_tools.py in the unreal-mcp repository with FastMCP tool wrappers.
"""
from pathlib import Path

target = Path('C:/Users/uraan/.reasonix/tools/unreal-mcp/Python/tools/editor_tools.py')
text = target.read_text(encoding='utf-8')

new_tools_code = '''
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
'''

# Update spawn_mesh_actor to support folder_path
old_spawn_mesh = '''    @mcp.tool()
    def spawn_mesh_actor(
        ctx: Context,
        mesh_path: str,
        name: str = "MeshActor",
        location: Optional[List[float]] = None,
        rotation: Optional[List[float]] = None,
        scale: Optional[List[float]] = None
    ) -> Dict[str, Any]:'''

new_spawn_mesh = '''    @mcp.tool()
    def spawn_mesh_actor(
        ctx: Context,
        mesh_path: str,
        name: str = "MeshActor",
        location: Optional[List[float]] = None,
        rotation: Optional[List[float]] = None,
        scale: Optional[List[float]] = None,
        folder_path: Optional[str] = None
    ) -> Dict[str, Any]:'''

assert old_spawn_mesh in text, 'old_spawn_mesh not found'
text = text.replace(old_spawn_mesh, new_spawn_mesh)
text = text.replace(
    '"scale": scale or [1.0, 1.0, 1.0]\n            }',
    '"scale": scale or [1.0, 1.0, 1.0],\n                "folder_path": folder_path\n            }'
)

# Insert new tools before logger.info("Editor tools registered successfully")
end_marker = '    logger.info("Editor tools registered successfully")'
assert end_marker in text, 'end_marker not found'
text = text.replace(end_marker, new_tools_code + '\n' + end_marker)

target.write_text(text, encoding='utf-8')
print('Successfully updated editor_tools.py with 6 new tools!')
