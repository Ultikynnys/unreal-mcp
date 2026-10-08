"""
Blueprint Tools for Unreal MCP.

This module provides tools for creating and manipulating Blueprint assets in Unreal Engine.
"""

import logging
from typing import Dict, List, Any
from mcp.server.fastmcp import FastMCP, Context
from tools.mcp_client import call_unreal, require_vectors3

# Get logger
logger = logging.getLogger("UnrealMCP")

def register_blueprint_tools(mcp: FastMCP):
    """Register Blueprint tools with the MCP server."""
    
    @mcp.tool()
    def create_blueprint(
        ctx: Context,
        name: str,
        parent_class: str
    ) -> Dict[str, Any]:
        """Create a new Blueprint class."""
        # Import inside function to avoid circular imports
        
        return call_unreal("create_blueprint", {
                "name": name,
                "parent_class": parent_class
            })
    
    @mcp.tool()
    def add_component_to_blueprint(
        ctx: Context,
        blueprint_name: str,
        component_type: str,
        component_name: str,
        location: List[float] = [],
        rotation: List[float] = [],
        scale: List[float] = [],
        component_properties: Dict[str, Any] = {}
    ) -> Dict[str, Any]:
        """
        Add a component to a Blueprint.
        
        Args:
            blueprint_name: Name of the target Blueprint
            component_type: Type of component to add (use component class name without U prefix)
            component_name: Name for the new component
            location: [X, Y, Z] coordinates for component's position
            rotation: [Pitch, Yaw, Roll] values for component's rotation
            scale: [X, Y, Z] values for component's scale
            component_properties: Additional properties to set on the component
        
        Returns:
            Information about the added component
        """
        params = {
            "blueprint_name": blueprint_name,
            "component_type": component_type,
            "component_name": component_name,
            "location": location or [0.0, 0.0, 0.0],
            "rotation": rotation or [0.0, 0.0, 0.0],
            "scale": scale or [1.0, 1.0, 1.0]
        }
        if component_properties:
            params["component_properties"] = component_properties
        error = require_vectors3(params, "location", "rotation", "scale")
        if error:
            return {"success": False, "message": error}
        return call_unreal("add_component_to_blueprint", params)
    
    @mcp.tool()
    def set_static_mesh_properties(
        ctx: Context,
        blueprint_name: str,
        component_name: str,
        static_mesh: str = "/Engine/BasicShapes/Cube.Cube"
    ) -> Dict[str, Any]:
        """
        Set static mesh properties on a StaticMeshComponent.
        
        Args:
            blueprint_name: Name of the target Blueprint
            component_name: Name of the StaticMeshComponent
            static_mesh: Path to the static mesh asset (e.g., "/Engine/BasicShapes/Cube.Cube")
            
        Returns:
            Response indicating success or failure
        """
        
        params = {
            "blueprint_name": blueprint_name,
            "component_name": component_name,
            "static_mesh": static_mesh
        }
        logger.info(f"Setting static mesh properties with params: {params}")
        return call_unreal("set_static_mesh_properties", params)
    
    @mcp.tool()
    def set_component_property(
        ctx: Context,
        blueprint_name: str,
        component_name: str,
        property_name: str,
        property_value,
    ) -> Dict[str, Any]:
        """Set a property on a component in a Blueprint."""
        
        params = {
            "blueprint_name": blueprint_name,
            "component_name": component_name,
            "property_name": property_name,
            "property_value": property_value
        }
        logger.info(f"Setting component property with params: {params}")
        return call_unreal("set_component_property", params)
    
    @mcp.tool()
    def set_physics_properties(
        ctx: Context,
        blueprint_name: str,
        component_name: str,
        simulate_physics: bool = True,
        gravity_enabled: bool = True,
        mass: float = 1.0,
        linear_damping: float = 0.01,
        angular_damping: float = 0.0
    ) -> Dict[str, Any]:
        """Set physics properties on a component."""
        
        params = {
            "blueprint_name": blueprint_name,
            "component_name": component_name,
            "simulate_physics": simulate_physics,
            "gravity_enabled": gravity_enabled,
            "mass": float(mass),
            "linear_damping": float(linear_damping),
            "angular_damping": float(angular_damping)
        }
        logger.info(f"Setting physics properties with params: {params}")
        return call_unreal("set_physics_properties", params)
    
    @mcp.tool()
    def compile_blueprint(
        ctx: Context,
        blueprint_name: str
    ) -> Dict[str, Any]:
        """Compile a Blueprint.

        Returns { compiled, num_errors, num_warnings, messages }. 'compiled' is true only
        when there are zero errors, so ALWAYS check num_errors / messages before treating
        a compile as successful.
        """
        
        params = {
            "blueprint_name": blueprint_name
        }
        logger.info(f"Compiling blueprint: {blueprint_name}")
        return call_unreal("compile_blueprint", params)

    @mcp.tool()
    def set_blueprint_property(
        ctx: Context,
        blueprint_name: str,
        property_name: str,
        property_value
    ) -> Dict[str, Any]:
        """
        Set a property on a Blueprint class default object.
        
        Args:
            blueprint_name: Name of the target Blueprint
            property_name: Name of the property to set
            property_value: Value to set the property to
            
        Returns:
            Response indicating success or failure
        """
        
        params = {
            "blueprint_name": blueprint_name,
            "property_name": property_name,
            "property_value": property_value
        }
        logger.info(f"Setting blueprint property with params: {params}")
        return call_unreal("set_blueprint_property", params)

    # @mcp.tool() commented out, just use set_component_property instead
    
    logger.info("Blueprint tools registered successfully") 