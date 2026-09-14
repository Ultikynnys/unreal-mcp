"""
Blueprint Node Tools for Unreal MCP.

This module provides tools for manipulating Blueprint graph nodes and connections.
"""

import logging
from typing import Dict, List, Any, Optional
from mcp.server.fastmcp import FastMCP, Context

# Get logger
logger = logging.getLogger("UnrealMCP")

def register_blueprint_node_tools(mcp: FastMCP):
    """Register Blueprint node manipulation tools with the MCP server."""
    
    @mcp.tool()
    def add_blueprint_event_node(
        ctx: Context,
        blueprint_name: str,
        event_name: str,
        node_position = None,
        graph_name: str = ""
    ) -> Dict[str, Any]:
        """
        Add an event node to a Blueprint's event graph.
        
        Args:
            blueprint_name: Name of the target Blueprint
            event_name: Name of the event. Use 'Receive' prefix for standard events:
                       - 'ReceiveBeginPlay' for Begin Play
                       - 'ReceiveTick' for Tick
                       - etc.
            node_position: Optional [X, Y] position in the graph
            graph_name: Optional graph name (defaults to 'EventGraph')
            
        Returns:
            Response containing the node ID and success status
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            # Handle default value within the method body
            if node_position is None:
                node_position = [0, 0]
            
            params = {
                "blueprint_name": blueprint_name,
                "event_name": event_name,
                "node_position": node_position
            }
            if graph_name:
                params["graph_name"] = graph_name
            
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            logger.info(f"Adding event node '{event_name}' to blueprint '{blueprint_name}'")
            response = unreal.send_command("add_blueprint_event_node", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Event node creation response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error adding event node: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
    
    @mcp.tool()
    def add_blueprint_input_action_node(
        ctx: Context,
        blueprint_name: str,
        action_name: str,
        node_position = None,
        graph_name: str = ""
    ) -> Dict[str, Any]:
        """
        Add an input action event node to a Blueprint's event graph.
        
        Args:
            blueprint_name: Name of the target Blueprint
            action_name: Name of the input action to respond to
            node_position: Optional [X, Y] position in the graph
            graph_name: Optional graph name (defaults to 'EventGraph')
            
        Returns:
            Response containing the node ID and success status
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            # Handle default value within the method body
            if node_position is None:
                node_position = [0, 0]
            
            params = {
                "blueprint_name": blueprint_name,
                "action_name": action_name,
                "node_position": node_position
            }
            if graph_name:
                params["graph_name"] = graph_name
            
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            logger.info(f"Adding input action node for '{action_name}' to blueprint '{blueprint_name}'")
            response = unreal.send_command("add_blueprint_input_action_node", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Input action node creation response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error adding input action node: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
    
    @mcp.tool()
    def add_blueprint_function_node(
        ctx: Context,
        blueprint_name: str,
        target: str,
        function_name: str,
        params = None,
        node_position = None,
        graph_name: str = ""
    ) -> Dict[str, Any]:
        """
        Add a function call node to a Blueprint's graph.
        
        Args:
            blueprint_name: Name of the target Blueprint
            target: Target object for the function (component name or self)
            function_name: Name of the function to call
            params: Optional parameters to set on the function node
            node_position: Optional [X, Y] position in the graph
            graph_name: Optional graph name (defaults to 'EventGraph'; can be 'UserConstructionScript' etc.)
            
        Returns:
            Response containing the node ID and success status
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            # Handle default values within the method body
            if params is None:
                params = {}
            if node_position is None:
                node_position = [0, 0]
            
            command_params = {
                "blueprint_name": blueprint_name,
                "target": target,
                "function_name": function_name,
                "params": params,
                "node_position": node_position
            }
            if graph_name:
                command_params["graph_name"] = graph_name
            
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            logger.info(f"Adding function node '{function_name}' to blueprint '{blueprint_name}'")
            response = unreal.send_command("add_blueprint_function_node", command_params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Function node creation response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error adding function node: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
            
    @mcp.tool()
    def connect_blueprint_nodes(
        ctx: Context,
        blueprint_name: str,
        source_node_id: str,
        source_pin: str,
        target_node_id: str,
        target_pin: str,
        graph_name: str = ""
    ) -> Dict[str, Any]:
        """
        Connect two nodes in a Blueprint's graph.
        
        Args:
            blueprint_name: Name of the target Blueprint
            source_node_id: ID of the source node
            source_pin: Name of the output pin on the source node
            target_node_id: ID of the target node
            target_pin: Name of the input pin on the target node
            graph_name: Optional graph name (can connect in 'UserConstructionScript', 'EventGraph', etc.)
            
        Returns:
            Response indicating success or failure
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            params = {
                "blueprint_name": blueprint_name,
                "source_node_id": source_node_id,
                "source_pin": source_pin,
                "target_node_id": target_node_id,
                "target_pin": target_pin
            }
            if graph_name:
                params["graph_name"] = graph_name
            
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            logger.info(f"Connecting nodes in blueprint '{blueprint_name}'")
            response = unreal.send_command("connect_blueprint_nodes", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Node connection response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error connecting nodes: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
    
    @mcp.tool()
    def add_blueprint_variable(
        ctx: Context,
        blueprint_name: str,
        variable_name: str,
        variable_type: str,
        is_exposed: bool = False,
        container: str = ""
    ) -> Dict[str, Any]:
        """
        Add a variable to a Blueprint.

        Args:
            blueprint_name: Name of the target Blueprint
            variable_name: Name of the variable
            variable_type: Element type (Boolean, Integer, Float, String, Vector)
            is_exposed: Whether to expose the variable to the editor
            container: Optional container type: "Array", "Set" or "Map"

        Returns:
            Response indicating success or failure
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            params = {
                "blueprint_name": blueprint_name,
                "variable_name": variable_name,
                "variable_type": variable_type,
                "is_exposed": is_exposed,
                "container": container
            }
            
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            logger.info(f"Adding variable '{variable_name}' to blueprint '{blueprint_name}'")
            response = unreal.send_command("add_blueprint_variable", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Variable creation response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error adding variable: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
    
    @mcp.tool()
    def add_blueprint_get_self_component_reference(
        ctx: Context,
        blueprint_name: str,
        component_name: str,
        node_position = None,
        graph_name: str = ""
    ) -> Dict[str, Any]:
        """
        Add a node that gets a reference to a component owned by the current Blueprint.
        This creates a node similar to what you get when dragging a component from the Components panel.
        
        Args:
            blueprint_name: Name of the target Blueprint
            component_name: Name of the component to get a reference to
            node_position: Optional [X, Y] position in the graph
            graph_name: Optional graph name (defaults to 'EventGraph')
            
        Returns:
            Response containing the node ID and success status
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            # Handle None case explicitly in the function
            if node_position is None:
                node_position = [0, 0]
            
            params = {
                "blueprint_name": blueprint_name,
                "component_name": component_name,
                "node_position": node_position
            }
            if graph_name:
                params["graph_name"] = graph_name
            
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            logger.info(f"Adding self component reference node for '{component_name}' to blueprint '{blueprint_name}'")
            response = unreal.send_command("add_blueprint_get_self_component_reference", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Self component reference node creation response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error adding self component reference node: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
    
    @mcp.tool()
    def add_blueprint_self_reference(
        ctx: Context,
        blueprint_name: str,
        node_position = None,
        graph_name: str = ""
    ) -> Dict[str, Any]:
        """
        Add a 'Get Self' node to a Blueprint's graph that returns a reference to this actor.
        
        Args:
            blueprint_name: Name of the target Blueprint
            node_position: Optional [X, Y] position in the graph
            graph_name: Optional graph name (defaults to 'EventGraph')
            
        Returns:
            Response containing the node ID and success status
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            if node_position is None:
                node_position = [0, 0]
                
            params = {
                "blueprint_name": blueprint_name,
                "node_position": node_position
            }
            if graph_name:
                params["graph_name"] = graph_name
            
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            logger.info(f"Adding self reference node to blueprint '{blueprint_name}'")
            response = unreal.send_command("add_blueprint_self_reference", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Self reference node creation response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error adding self reference node: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
    
    @mcp.tool()
    def find_blueprint_nodes(
        ctx: Context,
        blueprint_name: str,
        node_type = None,
        event_type = None,
        graph_name: str = ""
    ) -> Dict[str, Any]:
        """
        Find nodes in a Blueprint's graph.
        
        Args:
            blueprint_name: Name of the target Blueprint
            node_type: Optional type of node to find (Event, Function, FunctionEntry, Variable, etc.)
            event_type: Optional specific event type to find (BeginPlay, Tick, etc.)
            graph_name: Optional target graph name (e.g. 'UserConstructionScript', 'EventGraph', etc.)
            
        Returns:
            Response containing array of found node IDs, detailed node/pin information, and success status
        """
        from unreal_mcp_server import get_unreal_connection
        
        try:
            # C++ reads the event name from "event_name" (not "event_type").
            params = {
                "blueprint_name": blueprint_name,
                "node_type": node_type,
                "event_type": event_type,
                "event_name": event_type
            }
            if graph_name:
                params["graph_name"] = graph_name
            
            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            
            logger.info(f"Finding nodes in blueprint '{blueprint_name}'")
            response = unreal.send_command("find_blueprint_nodes", params)
            
            if not response:
                logger.error("No response from Unreal Engine")
                return {"success": False, "message": "No response from Unreal Engine"}
            
            logger.info(f"Node find response: {response}")
            return response
            
        except Exception as e:
            error_msg = f"Error finding nodes: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}
    
    @mcp.tool()
    def add_blueprint_node(
        ctx: Context,
        blueprint_name: str,
        node_type: str,
        params: Optional[Dict[str, Any]] = None,
        node_position = None,
        graph_name: str = ""
    ) -> Dict[str, Any]:
        """Add a control-flow / special node to a Blueprint's graph.

        node_type is one of:
            - "branch"       : if/then/else (condition + then/else exec)
            - "sequence"     : fan-out exec (params.num_outputs optional)
            - "cast"         : dynamic cast to a class (params.target_class required)
            - "custom_event" : a named custom event (params.event_name required)
            - "foreach"      : ForEachLoop macro (array element + loop body)
            - "spawn_actor"  : BeginDeferredActorSpawnFromClass (params.actor_class optional)
            - "variable_get" : get a variable (params.variable_name required)
            - "variable_set" : set a variable (params.variable_name required)
            - "make_transform": KismetMathLibrary::MakeTransform (Location/Rotation/Scale)
            - "break_struct" : UK2Node_BreakStruct (params.struct_type required, e.g. "Box", "Vector")
            - "make_struct"  : UK2Node_MakeStruct (params.struct_type required, e.g. "Vector", "Box")
            - "break_vector" / "break_box" : sugar for break_struct on Vector/Box
            - "make_vector"  / "make_box"  : sugar for make_struct on Vector/Box
            - "for_loop"     : standard ForLoop macro, index range (params.first_index/last_index optional)

        Optional params.defaults: { "<pin>": value } sets literals on the new node's pins.

        Args:
            blueprint_name: Name of the target Blueprint
            node_type: Type of node to create
            params: Parameters specific to the node type (e.g. {"struct_type": "Box"})
            node_position: Optional [X, Y] position in the graph
            graph_name: Optional graph name (defaults to 'EventGraph'; can be 'UserConstructionScript' etc.)

        Returns the node_id and its pins (name / direction / category) so they can be wired
        with connect_blueprint_nodes.
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            if params is None:
                params = {}
            if node_position is None:
                node_position = [0, 0]

            command_params = {
                "blueprint_name": blueprint_name,
                "node_type": node_type,
                "params": params,
                "node_position": node_position
            }
            if graph_name:
                command_params["graph_name"] = graph_name

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            logger.info(f"Adding '{node_type}' node to blueprint '{blueprint_name}'")
            response = unreal.send_command("add_blueprint_node", command_params)
            return response or {"success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error adding blueprint node: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def set_blueprint_node_pin_default(
        ctx: Context,
        blueprint_name: str,
        node_id: str,
        pin_name: str,
        value: Any,
        graph_name: str = ""
    ) -> Dict[str, Any]:
        """Set the literal/default value on an unconnected pin of a Blueprint node.

        Useful for non-zero constants (loop bounds, vector components, counts) without a
        separate literal node. `pin_name` may be a top-level pin ("LastIndex", "X") or a
        split struct member ("Vector.X", "ReturnValue_Max").

        Args:
            blueprint_name: Name of the target Blueprint
            node_id: Node GUID (from add_blueprint_node / find_blueprint_nodes)
            pin_name: Pin to set (e.g. "FirstIndex", "LastIndex", "X", "Vector.X")
            value: Literal value (string, number, or bool)
            graph_name: Optional graph name to narrow the node search

        Returns:
            Dict with success status and the applied value
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            params = {
                "blueprint_name": blueprint_name,
                "node_id": node_id,
                "pin_name": pin_name,
                "value": value
            }
            if graph_name:
                params["graph_name"] = graph_name

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            logger.info(f"Setting pin '{pin_name}' default on node '{node_id}' in '{blueprint_name}'")
            response = unreal.send_command("set_blueprint_node_pin_default", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error setting pin default: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def delete_blueprint_node(
        ctx: Context,
        blueprint_name: str,
        node_id: str,
        graph_name: str = ""
    ) -> Dict[str, Any]:
        """Delete a node from a Blueprint's graph.

        Args:
            blueprint_name: Name of the target Blueprint
            node_id: Node GUID or node name to delete
            graph_name: Optional graph name to narrow search

        Returns:
            Dict containing success status and deleted node information
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            params = {
                "blueprint_name": blueprint_name,
                "node_id": node_id
            }
            if graph_name:
                params["graph_name"] = graph_name

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            logger.info(f"Deleting node '{node_id}' from blueprint '{blueprint_name}'")
            response = unreal.send_command("delete_blueprint_node", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error deleting blueprint node: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def clear_blueprint_graph(
        ctx: Context,
        blueprint_name: str,
        graph_name: str = "UserConstructionScript",
        keep_entry_nodes: bool = True
    ) -> Dict[str, Any]:
        """Clear logic in a Blueprint graph (e.g. UserConstructionScript).

        Removes user-created nodes and breaks connections, while preserving the root
        entry node (e.g. FunctionEntry in UserConstructionScript) and returning its node_id
        so new logic can immediately be connected.

        Args:
            blueprint_name: Name of the target Blueprint
            graph_name: Name of graph to clear (defaults to 'UserConstructionScript')
            keep_entry_nodes: Whether to keep protected entry nodes (defaults to True)

        Returns:
            Dict containing success status, deleted_nodes_count, and entry_node_id
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            params = {
                "blueprint_name": blueprint_name,
                "graph_name": graph_name,
                "keep_entry_nodes": keep_entry_nodes
            }

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            logger.info(f"Clearing graph '{graph_name}' in blueprint '{blueprint_name}'")
            response = unreal.send_command("clear_blueprint_graph", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error clearing blueprint graph: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def disconnect_blueprint_pin(
        ctx: Context,
        blueprint_name: str,
        node_id: str,
        pin_name: str = "",
        target_node_id: str = "",
        target_pin_name: str = "",
        graph_name: str = ""
    ) -> Dict[str, Any]:
        """Disconnect pin connections on a Blueprint node.

        If only node_id is provided, breaks all links on the entire node.
        If pin_name is provided, breaks all links on that specific pin.
        If target_node_id and target_pin_name are provided, breaks only the link between those two pins.

        Args:
            blueprint_name: Name of the target Blueprint
            node_id: Node GUID or name
            pin_name: Optional specific pin name to disconnect
            target_node_id: Optional target node GUID
            target_pin_name: Optional target pin name
            graph_name: Optional graph name

        Returns:
            Dict containing success status
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            params = {
                "blueprint_name": blueprint_name,
                "node_id": node_id
            }
            if pin_name:
                params["pin_name"] = pin_name
            if target_node_id:
                params["target_node_id"] = target_node_id
            if target_pin_name:
                params["target_pin_name"] = target_pin_name
            if graph_name:
                params["graph_name"] = graph_name

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            logger.info(f"Disconnecting pin on node '{node_id}' in blueprint '{blueprint_name}'")
            response = unreal.send_command("disconnect_blueprint_pin", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error disconnecting blueprint pin: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    @mcp.tool()
    def get_blueprint_graphs(
        ctx: Context,
        blueprint_name: str
    ) -> Dict[str, Any]:
        """Get the list of all graphs in a Blueprint.

        Returns graph names, node counts, and flags indicating whether each graph
        is an EventGraph or UserConstructionScript.

        Args:
            blueprint_name: Name of the target Blueprint

        Returns:
            Dict containing list of graphs with name, node_count, is_construction_script, is_event_graph
        """
        from unreal_mcp_server import get_unreal_connection

        try:
            params = {
                "blueprint_name": blueprint_name
            }

            unreal = get_unreal_connection()
            if not unreal:
                logger.error("Failed to connect to Unreal Engine")
                return {"success": False, "message": "Failed to connect to Unreal Engine"}

            logger.info(f"Getting graphs for blueprint '{blueprint_name}'")
            response = unreal.send_command("get_blueprint_graphs", params)
            return response or {"success": False, "message": "No response from Unreal Engine"}

        except Exception as e:
            error_msg = f"Error getting blueprint graphs: {e}"
            logger.error(error_msg)
            return {"success": False, "message": error_msg}

    logger.info("Blueprint node tools registered successfully")