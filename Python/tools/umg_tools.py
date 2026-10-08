"""
UMG Tools for Unreal MCP.

This module provides tools for creating and manipulating UMG Widget Blueprints in Unreal Engine.
"""

import logging
from typing import Dict, List, Any
from mcp.server.fastmcp import FastMCP, Context
from tools.mcp_client import call_unreal

# Get logger
logger = logging.getLogger("UnrealMCP")

def register_umg_tools(mcp: FastMCP):
    """Register UMG tools with the MCP server."""

    @mcp.tool()
    def create_umg_widget_blueprint(
        ctx: Context,
        widget_name: str,
        parent_class: str = "UserWidget",
        path: str = "/Game/UI"
    ) -> Dict[str, Any]:
        """
        Create a new UMG Widget Blueprint.
        
        Args:
            widget_name: Name of the widget blueprint to create
            parent_class: Parent class for the widget (default: UserWidget)
            path: Content browser path where the widget should be created
            
        Returns:
            Dict containing success status and widget path
        """
        
        # The C++ handler reads the blueprint name from "name" (not "widget_name").
        params = {
            "name": widget_name,
            "widget_name": widget_name,
            "parent_class": parent_class,
            "path": path
        }
        logger.info(f"Creating UMG Widget Blueprint with params: {params}")
        return call_unreal("create_umg_widget_blueprint", params)

    @mcp.tool()
    def add_text_block_to_widget(
        ctx: Context,
        widget_name: str,
        text_block_name: str,
        text: str = "",
        position: List[float] = [0.0, 0.0],
        size: List[float] = [200.0, 50.0],
        font_size: int = 12,
        color: List[float] = [1.0, 1.0, 1.0, 1.0]
    ) -> Dict[str, Any]:
        """
        Add a Text Block widget to a UMG Widget Blueprint.
        
        Args:
            widget_name: Name of the target Widget Blueprint
            text_block_name: Name to give the new Text Block
            text: Initial text content
            position: [X, Y] position in the canvas panel
            size: [Width, Height] of the text block
            font_size: Font size in points
            color: [R, G, B, A] color values (0.0 to 1.0)
            
        Returns:
            Dict containing success status and text block properties
        """
        
        # C++ expects the target widget blueprint as "blueprint_name" and the
        # new child widget as "widget_name".
        params = {
            "blueprint_name": widget_name,
            "widget_name": text_block_name,
            "text_block_name": text_block_name,
            "text": text,
            "position": position,
            "size": size,
            "font_size": font_size,
            "color": color
        }
        logger.info(f"Adding Text Block to widget with params: {params}")
        return call_unreal("add_text_block_to_widget", params)

    @mcp.tool()
    def add_button_to_widget(
        ctx: Context,
        widget_name: str,
        button_name: str,
        text: str = "",
        position: List[float] = [0.0, 0.0],
        size: List[float] = [200.0, 50.0],
        font_size: int = 12,
        color: List[float] = [1.0, 1.0, 1.0, 1.0],
        background_color: List[float] = [0.1, 0.1, 0.1, 1.0]
    ) -> Dict[str, Any]:
        """
        Add a Button widget to a UMG Widget Blueprint.
        
        Args:
            widget_name: Name of the target Widget Blueprint
            button_name: Name to give the new Button
            text: Text to display on the button
            position: [X, Y] position in the canvas panel
            size: [Width, Height] of the button
            font_size: Font size for button text
            color: [R, G, B, A] text color values (0.0 to 1.0)
            background_color: [R, G, B, A] button background color values (0.0 to 1.0)
            
        Returns:
            Dict containing success status and button properties
        """
        
        # C++ expects the target widget blueprint as "blueprint_name" and the
        # new button child as "widget_name".
        params = {
            "blueprint_name": widget_name,
            "widget_name": button_name,
            "button_name": button_name,
            "text": text,
            "position": position,
            "size": size,
            "font_size": font_size,
            "color": color,
            "background_color": background_color
        }
        logger.info(f"Adding Button to widget with params: {params}")
        return call_unreal("add_button_to_widget", params)

    @mcp.tool()
    def bind_widget_event(
        ctx: Context,
        widget_name: str,
        widget_component_name: str,
        event_name: str,
        function_name: str = ""
    ) -> Dict[str, Any]:
        """
        Bind an event on a widget component to a function.
        
        Args:
            widget_name: Name of the target Widget Blueprint
            widget_component_name: Name of the widget component (button, etc.)
            event_name: Name of the event to bind (OnClicked, etc.)
            function_name: Name of the function to create/bind to (defaults to f"{widget_component_name}_{event_name}")
            
        Returns:
            Dict containing success status and binding information
        """
        
        # If no function name provided, create one from component and event names
        if not function_name:
            function_name = f"{widget_component_name}_{event_name}"
        # C++ expects the target widget blueprint as "blueprint_name" and the
        # component to bind as "widget_name".
        params = {
            "blueprint_name": widget_name,
            "widget_name": widget_component_name,
            "widget_component_name": widget_component_name,
            "event_name": event_name,
            "function_name": function_name
        }
        logger.info(f"Binding widget event with params: {params}")
        return call_unreal("bind_widget_event", params)

    @mcp.tool()
    def add_widget_to_viewport(
        ctx: Context,
        widget_name: str,
        z_order: int = 0
    ) -> Dict[str, Any]:
        """
        Add a Widget Blueprint instance to the viewport.
        
        Args:
            widget_name: Name of the Widget Blueprint to add
            z_order: Z-order for the widget (higher numbers appear on top)
            
        Returns:
            Dict containing success status and widget instance information
        """
        
        # C++ expects the target widget blueprint as "blueprint_name".
        params = {
            "blueprint_name": widget_name,
            "widget_name": widget_name,
            "z_order": z_order
        }
        logger.info(f"Adding widget to viewport with params: {params}")
        return call_unreal("add_widget_to_viewport", params)

    @mcp.tool()
    def set_text_block_binding(
        ctx: Context,
        widget_name: str,
        text_block_name: str,
        binding_property: str,
        binding_type: str = "Text"
    ) -> Dict[str, Any]:
        """
        Set up a property binding for a Text Block widget.
        
        Args:
            widget_name: Name of the target Widget Blueprint
            text_block_name: Name of the Text Block to bind
            binding_property: Name of the property to bind to
            binding_type: Type of binding (Text, Visibility, etc.)
            
        Returns:
            Dict containing success status and binding information
        """
        
        # C++ expects the widget blueprint as "blueprint_name", the text block as
        # "widget_name", and the binding variable as "binding_name".
        params = {
            "blueprint_name": widget_name,
            "widget_name": text_block_name,
            "text_block_name": text_block_name,
            "binding_name": binding_property,
            "binding_property": binding_property,
            "binding_type": binding_type
        }
        logger.info(f"Setting text block binding with params: {params}")
        return call_unreal("set_text_block_binding", params)

    logger.info("UMG tools registered successfully") 