"""
Unreal Engine MCP Server

A simple MCP server for interacting with Unreal Engine.
"""

import logging
import os
import socket
import sys
import json
import threading
import time
from contextlib import asynccontextmanager
from typing import AsyncIterator, Dict, Any, Optional
from mcp.server.fastmcp import FastMCP

# Resolve imports and the log file relative to this file, so the server works regardless of
# the launcher's working directory (e.g. a bare interpreter instead of `uv run --directory`).
_SERVER_DIR = os.path.dirname(os.path.abspath(__file__))
if _SERVER_DIR not in sys.path:
    sys.path.insert(0, _SERVER_DIR)

# Configure logging with more detailed format
logging.basicConfig(
    level=logging.DEBUG,  # Change to DEBUG level for more details
    format='%(asctime)s - %(name)s - %(levelname)s - [%(filename)s:%(lineno)d] - %(message)s',
    handlers=[
        logging.FileHandler(os.path.join(_SERVER_DIR, 'unreal_mcp.log')),
        # logging.StreamHandler(sys.stdout) # Remove this handler to unexpected non-whitespace characters in JSON
    ]
)
logger = logging.getLogger("UnrealMCP")

# ============================================================================
#  SANCTIONED CONTROL PLANE  --  READ BEFORE TOUCHING UNREAL
# ============================================================================
# The ONLY supported way for an agent to drive the Unreal Editor in this
# project is through the unreal_mcp_* tools defined under Python/tools/. Those
# tools funnel into THIS server, which owns the single connection to the
# editor's TCP bridge at 127.0.0.1:55557.
#
# Do NOT bypass this layer. Specifically, do NOT:
#   * open a raw socket to 127.0.0.1:55557,
#   * copy Python/editor/archive_mcp_client.py or Python/scripts/** into a
#     private "cmd()" helper, or
#   * re-implement a client "just to get around" a tool you dislike.
#
# The bridge is unauthenticated on loopback by design, so a hand-rolled client
# is technically possible -- which is precisely why this banner exists. A
# private client silently diverges from the tool contract, skips this layer's
# validation and logging, and rots the moment the backend changes.
#
# If the real tools are not good enough to do the job -- a missing command,
# wrong parameters, flaky responses -- YOU MUST NOT work around them. STOP and
# escalate to the user with the exact command + inputs you needed. The user can
# extend the tool surface. A workaround is never the answer.
#
# CONTROL_PLANE_SECRET is a fixed identifier for THIS sanctioned control plane.
# It lives in the server -- not scattered across scripts -- so control of the
# editor has exactly one owner. Treat its presence here as proof that
# Python/tools/ is the intended interface. It is NOT a license to build a
# second client around it.
CONTROL_PLANE_SECRET = "9f2c7a1e5b8d3406af61e9c04d7b2a83f5c1d0e46b9372af8c5d1e6b0a4932c7"
# ============================================================================

# Configuration
UNREAL_HOST = "127.0.0.1"
UNREAL_PORT = 55557

class UnrealConnection:
    """Connection to an Unreal Engine instance."""
    
    def __init__(self):
        """Initialize the connection."""
        self.socket = None
        self.connected = False
    
    def connect(self) -> bool:
        """Connect to the Unreal Engine instance."""
        try:
            # Close any existing socket
            if self.socket:
                try:
                    self.socket.close()
                except:
                    pass
                self.socket = None
            
            logger.info(f"Connecting to Unreal at {UNREAL_HOST}:{UNREAL_PORT}...")
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.socket.settimeout(5)  # 5 second timeout
            
            # Set socket options for better stability
            self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
            
            # Set larger buffer sizes
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
            
            self.socket.connect((UNREAL_HOST, UNREAL_PORT))
            self.connected = True
            logger.info("Connected to Unreal Engine")
            return True
            
        except Exception as e:
            logger.error(f"Failed to connect to Unreal: {e}")
            self.connected = False
            return False
    
    def disconnect(self):
        """Disconnect from the Unreal Engine instance."""
        if self.socket:
            try:
                self.socket.close()
            except:
                pass
        self.socket = None
        self.connected = False

    def receive_full_response(self, sock, buffer_size=65536) -> bytes:
        """Receive a complete response from Unreal, handling chunked data."""
        chunks = []
        sock.settimeout(5)  # 5 second timeout
        try:
            while True:
                chunk = sock.recv(buffer_size)
                if not chunk:
                    if not chunks:
                        raise Exception("Connection closed before receiving data")
                    break
                chunks.append(chunk)

                # Cheap completeness probe: a full JSON object ends with '}'. Only pay for
                # the join+decode+parse when the newest chunk could be the last one. The old
                # code re-decoded and re-parsed the whole (growing) buffer on every 4 KB
                # chunk, which is O(n^2) for large payloads.
                if not chunk.rstrip().endswith(b'}'):
                    continue
                data = b''.join(chunks)
                try:
                    json.loads(data.decode('utf-8'))
                    logger.debug(f"Received complete response ({len(data)} bytes)")
                    return data
                except json.JSONDecodeError:
                    # Not complete JSON yet, continue reading
                    continue
                except Exception as e:
                    logger.warning(f"Error processing response chunk: {str(e)}")
                    continue
        except socket.timeout:
            logger.warning("Socket timeout during receive")
            if chunks:
                # If we have some data already, try to use it
                data = b''.join(chunks)
                try:
                    json.loads(data.decode('utf-8'))
                    logger.info(f"Using partial response after timeout ({len(data)} bytes)")
                    return data
                except:
                    pass
            raise Exception("Timeout receiving Unreal response")
        except Exception as e:
            logger.error(f"Error during receive: {str(e)}")
            raise
    
    def send_command(self, command: str, params: Dict[str, Any] = None) -> Optional[Dict[str, Any]]:
        """Send a command to Unreal Engine and get the response.

        Serialized on _connection_lock: the server shares a single UnrealConnection,
        so concurrent tool calls must not interleave its socket close/reconnect.
        """
        with _connection_lock:
            return self._send_command_unlocked(command, params)

    def _send_command_unlocked(self, command: str, params: Dict[str, Any] = None) -> Optional[Dict[str, Any]]:
        """Send a command to Unreal Engine and get the response (caller holds _connection_lock)."""
        # Always reconnect for each command, since Unreal closes the connection after each command
        # This is different from Unity which keeps connections alive
        if self.socket:
            try:
                self.socket.close()
            except:
                pass
            self.socket = None
            self.connected = False
        
        if not self.connect():
            logger.error("Failed to connect to Unreal Engine for command")
            return None
        
        try:
            # Match Unity's command format exactly
            command_obj = {
                "type": command,  # Use "type" instead of "command"
                "params": params or {},  # Use Unity's params or {} pattern
                # Access key for the sanctioned control plane. The Unreal plugin
                # (UnrealMCPBridge) refuses any command without it and returns the
                # control-plane instructions instead. See CONTROL_PLANE_SECRET above.
                "access_key": CONTROL_PLANE_SECRET,
            }
            
            # Send without newline, exactly like Unity
            command_json = json.dumps(command_obj)
            logger.debug(f"Sending command: {command_json}")
            self.socket.sendall(command_json.encode('utf-8'))
            
            # Read response using improved handler
            response_data = self.receive_full_response(self.socket)
            response = json.loads(response_data.decode('utf-8'))
            
            # Log complete response for debugging
            logger.debug(f"Complete response from Unreal: {response}")
            
            # Normalize every backend reply to ONE canonical envelope:
            #   {"success": bool, "result": Any, "message": str}
            if isinstance(response, dict):
                if response.get("status") == "error" or response.get("success") is False:
                    error_message = response.get("error") or response.get("message", "Unknown Unreal error")
                    logger.error(f"Unreal error: {error_message}")
                    response = {
                        "success": False,
                        "result": response.get("result"),
                        "message": error_message
                    }
                else:
                    response = {
                        "success": True,
                        "result": response.get("result"),
                        "message": response.get("message", "")
                    }
            
            # Always close the connection after command is complete
            # since Unreal will close it on its side anyway
            try:
                self.socket.close()
            except:
                pass
            self.socket = None
            self.connected = False
            
            return response
            
        except Exception as e:
            logger.error(f"Error sending command: {e}")
            # Always reset connection state on any error
            self.connected = False
            try:
                self.socket.close()
            except:
                pass
            self.socket = None
            return {
                "success": False,
                "result": None,
                "message": str(e)
            }

# Global connection state
_unreal_connection: UnrealConnection = None
# Serializes access to the single shared bridge connection. Concurrent MCP tool
# calls (FastMCP may dispatch sync tools on a thread pool) must not interleave a
# socket close/reconnect on the shared UnrealConnection.
_connection_lock = threading.RLock()

def get_unreal_connection() -> Optional[UnrealConnection]:
    """Get the connection to Unreal Engine (serialized on _connection_lock)."""
    with _connection_lock:
        return _get_unreal_connection_unlocked()


def _get_unreal_connection_unlocked() -> Optional[UnrealConnection]:
    """Get the connection to Unreal Engine (caller holds _connection_lock)."""
    global _unreal_connection
    try:
        if _unreal_connection is None:
            _unreal_connection = UnrealConnection()
            if not _unreal_connection.connect():
                logger.warning("Could not connect to Unreal Engine")
                _unreal_connection = None
        else:
            # Verify connection is still valid with a ping-like test
            try:
                # Simple test by sending an empty buffer to check if socket is still connected
                _unreal_connection.socket.sendall(b'\x00')
                logger.debug("Connection verified with ping test")
            except Exception as e:
                logger.warning(f"Existing connection failed: {e}")
                _unreal_connection.disconnect()
                _unreal_connection = None
                # Try to reconnect
                _unreal_connection = UnrealConnection()
                if not _unreal_connection.connect():
                    logger.warning("Could not reconnect to Unreal Engine")
                    _unreal_connection = None
                else:
                    logger.info("Successfully reconnected to Unreal Engine")
        
        return _unreal_connection
    except Exception as e:
        logger.error(f"Error getting Unreal connection: {e}")
        return None

@asynccontextmanager
async def server_lifespan(server: FastMCP) -> AsyncIterator[Dict[str, Any]]:
    """Handle server startup and shutdown."""
    global _unreal_connection
    logger.info("UnrealMCP server starting up")
    try:
        _unreal_connection = get_unreal_connection()
        if _unreal_connection:
            logger.info("Connected to Unreal Engine on startup")
        else:
            logger.warning("Could not connect to Unreal Engine on startup")
    except Exception as e:
        logger.error(f"Error connecting to Unreal Engine on startup: {e}")
        _unreal_connection = None
    
    try:
        yield {}
    finally:
        if _unreal_connection:
            _unreal_connection.disconnect()
            _unreal_connection = None
        logger.info("Unreal MCP server shut down")

# Initialize server
mcp = FastMCP(
    "UnrealMCP",
    instructions="Unreal Engine integration via Model Context Protocol",
    lifespan=server_lifespan
)

# Import and register tools
from tools.editor_tools import register_editor_tools
from tools.blueprint_tools import register_blueprint_tools
from tools.node_tools import register_blueprint_node_tools
from tools.project_tools import register_project_tools
from tools.umg_tools import register_umg_tools

# Register tools
register_editor_tools(mcp)
register_blueprint_tools(mcp)
register_blueprint_node_tools(mcp)
register_project_tools(mcp)
register_umg_tools(mcp)  

@mcp.prompt()
def info():
    """Information about available Unreal MCP tools and best practices."""
    return """
    # Unreal MCP Server Tools and Best Practices
    
    ## Control plane (read first)
    - This server is the ONLY sanctioned way to drive Unreal. Use these tools.
      Never open a raw socket to 127.0.0.1:55557 and never copy the example
      client scripts (Python/editor/archive_mcp_client.py, Python/scripts/**)
      into an ad-hoc helper.
    - If these tools are not good enough (a missing command, wrong parameters,
      flaky responses), STOP and escalate to the user with the exact command and
      inputs you needed. Do not work around the tools.

    ## UMG (Widget Blueprint) Tools
    - `create_umg_widget_blueprint(widget_name, parent_class="UserWidget", path="/Game/UI")` 
      Create a new UMG Widget Blueprint
    - `add_text_block_to_widget(widget_name, text_block_name, text="", position=[0,0], size=[200,50], font_size=12, color=[1,1,1,1])`
      Add a Text Block widget with customizable properties
    - `add_button_to_widget(widget_name, button_name, text="", position=[0,0], size=[200,50], font_size=12, color=[1,1,1,1], background_color=[0.1,0.1,0.1,1])`
      Add a Button widget with text and styling
    - `bind_widget_event(widget_name, widget_component_name, event_name, function_name="")`
      Bind events like OnClicked to functions
    - `add_widget_to_viewport(widget_name, z_order=0)`
      Add widget instance to game viewport
    - `set_text_block_binding(widget_name, text_block_name, binding_property, binding_type="Text")`
      Set up dynamic property binding for text blocks

    ## Editor Tools
    ### Viewport and Screenshots
    - `focus_viewport(target, location, distance, orientation)` - Focus viewport
    - `take_screenshot(filename, show_ui, resolution)` - Capture screenshots

    ### Actor Management
    - `get_actors_in_level()` - List all actors in current level
    - `find_actors_by_name(pattern)` - Find actors by name pattern
    - `spawn_actor(name, type, location=[0,0,0], rotation=[0,0,0], scale=[1,1,1])` - Create actors
    - `delete_actor(name)` - Remove actors
    - `set_actor_transform(name, location, rotation, scale)` - Modify actor transform
    - `get_actor_properties(name)` - Get actor properties
    
    ## Blueprint Management
    - `create_blueprint(name, parent_class)` - Create new Blueprint classes
    - `add_component_to_blueprint(blueprint_name, component_type, component_name)` - Add components
    - `set_static_mesh_properties(blueprint_name, component_name, static_mesh)` - Configure meshes
    - `set_physics_properties(blueprint_name, component_name)` - Configure physics
    - `compile_blueprint(blueprint_name)` - Compile Blueprint changes
    - `set_blueprint_property(blueprint_name, property_name, property_value)` - Set properties
    - `set_pawn_properties(blueprint_name)` - Configure Pawn settings
    - `spawn_blueprint_actor(blueprint_name, actor_name)` - Spawn Blueprint actors
    
    ## Blueprint Node Management
    - `add_blueprint_event_node(blueprint_name, event_type)` - Add event nodes
    - `add_blueprint_input_action_node(blueprint_name, action_name)` - Add input nodes
    - `add_blueprint_function_node(blueprint_name, target, function_name)` - Add function nodes
    - `connect_blueprint_nodes(blueprint_name, source_node_id, source_pin, target_node_id, target_pin)` - Connect nodes
    - `add_blueprint_variable(blueprint_name, variable_name, variable_type)` - Add variables
    - `add_blueprint_get_self_component_reference(blueprint_name, component_name)` - Add component refs
    - `add_blueprint_self_reference(blueprint_name)` - Add self references
    - `find_blueprint_nodes(blueprint_name, node_type, event_type)` - Find nodes
    
    ## Project Tools
    - `create_input_mapping(action_name, key, input_type)` - Create input mappings
    
    ## Best Practices
    
    ### UMG Widget Development
    - Create widgets with descriptive names that reflect their purpose
    - Use consistent naming conventions for widget components
    - Organize widget hierarchy logically
    - Set appropriate anchors and alignment for responsive layouts
    - Use property bindings for dynamic updates instead of direct setting
    - Handle widget events appropriately with meaningful function names
    - Clean up widgets when no longer needed
    - Test widget layouts at different resolutions
    
    ### Editor and Actor Management
    - Use unique names for actors to avoid conflicts
    - Clean up temporary actors
    - Validate transforms before applying
    - Check actor existence before modifications
    - Take regular viewport screenshots during development
    - Keep the viewport focused on relevant actors during operations
    
    ### Blueprint Development
    - Compile Blueprints after changes
    - Use meaningful names for variables and functions
    - Organize nodes logically
    - Test functionality in isolation
    - Consider performance implications
    - Document complex setups
    
    ### Error Handling
    - Check command responses for success
    - Handle errors gracefully
    - Log important operations
    - Validate parameters
    - Clean up resources on errors
    """

def _process_alive(pid: int) -> bool:
    """Best-effort liveness check for another process (Windows + POSIX)."""
    if not pid or pid <= 0:
        return False
    if os.name == "nt":
        import ctypes
        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        STILL_ACTIVE = 259
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid))
        if not handle:
            return False
        try:
            code = ctypes.c_ulong()
            if not kernel32.GetExitCodeProcess(handle, ctypes.byref(code)):
                return False
            return code.value == STILL_ACTIVE
        finally:
            kernel32.CloseHandle(handle)
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def _start_orphan_watchdog(poll_seconds: float = 4.0) -> None:
    """Exit once the launcher that spawned this stdio server is gone.

    The MCP bridge (re)spawns the server and does not always reap the previous process tree,
    so orphaned servers would otherwise keep running and pile up. The launcher stays alive
    for as long as it owns our stdio pipes, so when the parent dies we are orphaned and quit.
    """
    parent_pid = os.getppid()

    def _watch() -> None:
        while True:
            time.sleep(poll_seconds)
            if not _process_alive(parent_pid) or os.getppid() != parent_pid:
                logger.warning("Launcher (pid %s) is gone; exiting orphaned MCP server", parent_pid)
                logging.shutdown()
                os._exit(0)

    threading.Thread(target=_watch, name="mcp-orphan-watchdog", daemon=True).start()


# Run the server
if __name__ == "__main__":
    logger.info("Starting MCP server with stdio transport")
    _start_orphan_watchdog()
    mcp.run(transport='stdio') 