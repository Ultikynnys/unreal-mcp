"""
Unreal Engine MCP Server

A simple MCP server for interacting with Unreal Engine.
"""

import logging
import os
import pathlib
import re
import socket
import subprocess
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
#   * copy the example scripts under Python/scripts/ into a private "cmd()" helper, or
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
UNREAL_TIMEOUT = int(os.getenv("UNREAL_MCP_TIMEOUT", "60"))
# Timeout for a single recv() while awaiting a response. Editor operations run
# synchronously on the game thread, so a short read timeout abandons an operation
# that is still running (map loads, batch renames). Keep this generous and separate
# from the connect timeout above.
UNREAL_READ_TIMEOUT = int(os.getenv("UNREAL_MCP_READ_TIMEOUT", "600"))

# --- Revision handshake -----------------------------------------------------
# Identity is the commit, not a hand-incremented number: the plugin bakes in the commit it was
# BUILT from (UnrealMCP.Build.cs -> MCP_REVISION) and this server uses the commit it runs from,
# so any drift between the two is a mismatch and nobody has to remember to bump a constant. The
# checkout is assumed to be a git repository.
REPO_ROOT = pathlib.Path(_SERVER_DIR).parent


def repo_revision() -> str:
    """The commit this server runs from. UNREAL_MCP_REVISION wins, for a packaged or copied tree."""
    override = os.getenv("UNREAL_MCP_REVISION")
    if override:
        return override.strip()
    try:
        result = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "rev-parse", "HEAD"],
            capture_output=True, text=True, timeout=10,
        )
    except (OSError, subprocess.SubprocessError) as error:
        logger.error("Could not read the git revision: %s", error)
        return ""
    return result.stdout.strip() if result.returncode == 0 else ""


REPO_REVISION = repo_revision()

_dirty_warned = False


def _warn_built_dirty_once(revision: str) -> None:
    """A dirty build's commit does not identify the code it runs; say so once per process."""
    global _dirty_warned
    if not _dirty_warned:
        _dirty_warned = True
        logger.warning(
            "The running plugin was built from a dirty tree (revision %s), so its revision does "
            "not identify the code it is running; rebuild from a clean tree before trusting it",
            revision or "unknown")


def revision_mismatch_error(reported: str) -> str:
    """'' when the running plugin was built from this revision, otherwise the refusal.

    Three refusals, all fail-closed: a different commit, no revision field at all (a plugin
    built before the handshake), and "unknown" (a plugin built where git was unavailable).
    """
    if not REPO_REVISION:
        return (
            "Unreal MCP revision unknown: this server cannot tell which commit it is running "
            "from (no git, or not a checkout). Set UNREAL_MCP_REVISION to the shipped revision, "
            "or run the server from a git checkout; every call fails until the two sides can be "
            "compared."
        )
    if reported == REPO_REVISION:
        return ""
    if not reported:
        detail = ("the running Unreal plugin reported no revision at all, so it was built before "
                  "the handshake existed")
    elif reported == "unknown":
        detail = ("the running Unreal plugin was built without a revision (git unavailable at "
                  "build time, or MCP_REVISION unset)")
    else:
        detail = f"the running plugin reports {reported}"
    return (
        f"Unreal MCP revision mismatch: this checkout is {REPO_REVISION}, {detail}. The running "
        f"plugin is stale: rebuild it and restart the editor before calling again. Rebuild with "
        f'"<UE>\\Engine\\Build\\BatchFiles\\Build.bat" MCPGameProjectEditor Win64 Development '
        f'"MCPGameProject/MCPGameProject.uproject" -WaitMutex, then run '
        f"`uv run --project Python python Python/scripts/editor_process.py restart`. "
        f"A plain rebuild can answer 'up to date' and keep the old revision, so touch "
        f"MCPGameProject/Plugins/UnrealMCP/Source/UnrealMCP/UnrealMCP.Build.cs first (or do a "
        f"clean build). "
        f"Every call fails until the plugin matches; retrying the same call will not help."
    )


def _failure_detail(response: Dict[str, Any]) -> str:
    """Best-effort failure reason built from a reply's own detail fields.

    Used when neither 'error' nor 'message' carries a reason, so the caller never gets a
    bare "Unknown Unreal error" while the reply actually holds useful detail (a Python
    trace in 'output', a per-item message in 'results', ...).
    """
    output = response.get("output")
    if isinstance(output, list) and output:
        text = " ".join(str(line) for line in output).strip()
        if text:
            return text[:1000]
    results = response.get("results")
    if isinstance(results, list):
        for item in results:
            if isinstance(item, dict) and item.get("success") is False:
                reason = item.get("error") or item.get("message")
                if reason:
                    command = item.get("command", "action")
                    return f"{command}: {reason}"
    return ""


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
            self.socket.settimeout(UNREAL_TIMEOUT)
            
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
        sock.settimeout(UNREAL_READ_TIMEOUT)
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
            logger.warning("Socket timeout during receive after %ss", UNREAL_READ_TIMEOUT)
            if chunks:
                # If we have some data already, try to use it
                data = b''.join(chunks)
                try:
                    json.loads(data.decode('utf-8'))
                    logger.info(f"Using partial response after timeout ({len(data)} bytes)")
                    return data
                except Exception:
                    pass
            raise Exception(
                f"No response from Unreal within {UNREAL_READ_TIMEOUT}s. The editor may "
                f"still be working on this operation - long map loads/saves keep running "
                f"on the game thread after the read timeout. Check the editor state before "
                f"retrying, and raise UNREAL_MCP_READ_TIMEOUT if this duration is expected."
            )
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

    def _dispatch(self, command: str, params: Dict[str, Any] = None) -> Optional[Dict[str, Any]]:
        """One request/response over a fresh socket: connect, send, read, parse, close.

        Returns the parsed reply, and raises when the editor cannot be reached, so a
        transport failure is never mistaken for a protocol problem. It carries no version
        logic: the caller decides what a reply means.
        """
        # Always reconnect: the bridge closes the socket after each reply, so there is no
        # persistent connection to reuse.
        if self.socket:
            try:
                self.socket.close()
            except:
                pass
            self.socket = None
            self.connected = False

        if not self.connect():
            raise RuntimeError(
                f"Could not connect to the Unreal bridge at {UNREAL_HOST}:{UNREAL_PORT} "
                f"(no editor listening, or the UnrealMCP plugin is not loaded)")

        try:
            command_obj = {
                "type": command,
                "params": params or {},
                # Access key for the sanctioned control plane. UnrealMCPBridge refuses any
                # command without it and returns the control-plane instructions instead.
                "access_key": CONTROL_PLANE_SECRET,
            }
            command_json = json.dumps(command_obj)
            logger.debug(f"Sending command: {command_json}")
            self.socket.sendall(command_json.encode('utf-8'))

            response_data = self.receive_full_response(self.socket)
            response = json.loads(response_data.decode('utf-8'))
            logger.debug(f"Complete response from Unreal: {response}")
            return response
        finally:
            self.connected = False
            try:
                self.socket.close()
            except:
                pass
            self.socket = None

    def _probe_reply(self) -> Dict[str, Any]:
        """The `ping` reply, used as the handshake: it touches no UObjects. It raises if the
        editor cannot be reached - a dead editor is a connection problem, not drift."""
        reply = self._dispatch("ping")
        return reply if isinstance(reply, dict) else {}

    def _send_command_unlocked(self, command: str, params: Dict[str, Any] = None) -> Optional[Dict[str, Any]]:
        """Send a command to Unreal Engine and get the response (caller holds _connection_lock)."""
        try:
            # Handshake FIRST. Checking the real command's own reply would be too late: the
            # editor executes a command before it replies, so a stale plugin could still
            # create or delete assets while we merely refused to report the result. Probe
            # with a ping (which changes nothing) and refuse before dispatching anything.
            if command != "ping":
                probe = self._probe_reply()
                mismatch = revision_mismatch_error(str(probe.get("revision") or ""))
                if mismatch:
                    logger.error("Refusing '%s' before dispatch: %s", command, mismatch)
                    return {"success": False, "result": None, "message": mismatch,
                            "code": "REVISION_MISMATCH"}

            response = self._dispatch(command, params)

            # Belt and braces: a reply that disagrees with the handshake is refused too, in
            # case the editor was swapped for a different build between the two round trips.
            reported = str(response.get("revision") or "") if isinstance(response, dict) else ""
            mismatch = revision_mismatch_error(reported)
            if mismatch:
                logger.error("Refusing command '%s': %s", command, mismatch)
                return {"success": False, "result": None, "message": mismatch,
                        "code": "REVISION_MISMATCH"}
            if isinstance(response, dict) and response.get("built_dirty"):
                _warn_built_dirty_once(reported)

            # Normalize every backend reply to ONE canonical envelope:
            #   {"success": bool, "result": Any, "message": str}
            # Original fields are preserved rather than replaced, so a failure can still be
            # inspected (execute_python's "output" trace, batch "results", ...) instead of
            # collapsing to a bare error with the detail discarded.
            if isinstance(response, dict):
                envelope = dict(response)
                if response.get("status") == "error" or response.get("success") is False:
                    error_message = (
                        response.get("error")
                        or response.get("message")
                        or _failure_detail(response)
                        or "Unknown Unreal error"
                    )
                    logger.error(f"Unreal error: {error_message}")
                    envelope["success"] = False
                    envelope["message"] = error_message
                else:
                    envelope["success"] = True
                    envelope["result"] = response.get("result")
                    envelope["message"] = response.get("message", "")
                response = envelope

            return response

        except Exception as e:
            logger.error(f"Error sending command: {e}")
            return {"success": False, "result": None, "message": str(e)}


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
            # send_command() reconnects for every command (the bridge closes the socket
            # after each reply), so there is no persistent socket to validate here.
            # Probing by writing a byte would corrupt the next command's stream.
            logger.debug("Reusing existing Unreal connection handle")

        return _unreal_connection
    except Exception as e:
        logger.error(f"Error getting Unreal connection: {e}")
        return None

@asynccontextmanager
async def server_lifespan(server: FastMCP) -> AsyncIterator[Dict[str, Any]]:
    """Handle server startup and shutdown."""
    global _unreal_connection
    logger.info("UnrealMCP server starting up")
    logger.info("This server runs from revision %s", REPO_REVISION or "UNKNOWN (no git checkout)")
    if not REPO_REVISION:
        logger.warning("Cannot determine this server's revision. Set UNREAL_MCP_REVISION, or run "
                       "from a git checkout: every call is refused until the plugin and this side "
                       "can be compared.")
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

# The agent-facing documentation of the tool surface is generated from the tools themselves
# (Python/tool_catalog.py), so it cannot drift. The hand-written list it replaces named a
# commented-out tool and parameters that no longer exist, and omitted most of the surface.
from tool_catalog import discover_tools, render_instructions, render_tool_reference

TOOL_CATALOG = discover_tools(pathlib.Path(_SERVER_DIR) / "tools")

# Initialize server
mcp = FastMCP(
    "UnrealMCP",
    instructions=render_instructions(TOOL_CATALOG),
    lifespan=server_lifespan
)

# Import and register tools
from tools.editor_tools import register_editor_tools
from tools.blueprint_tools import register_blueprint_tools
from tools.node_tools import register_blueprint_node_tools
from tools.project_tools import register_project_tools
from tools.umg_tools import register_umg_tools
from tools.asset_tools import register_asset_tools

# Register tools
register_editor_tools(mcp)
register_blueprint_tools(mcp)
register_blueprint_node_tools(mcp)
register_project_tools(mcp)
register_umg_tools(mcp)
register_asset_tools(mcp)  

@mcp.prompt()
def info():
    """The full Unreal MCP tool reference, generated from the registered tools."""
    return render_tool_reference(TOOL_CATALOG)


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