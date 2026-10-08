"""The single call path every MCP tool uses.

Each tool used to repeat the same four blocks: import the connection, guard a missing
connection, guard an empty reply, and wrap the whole call in try/except. That is roughly ten
lines of identical plumbing per tool, 78 times over. This module owns it, so a tool body is
just the parameters it assembles and the reply it returns.

The command name stays a literal at the call site - ``call_unreal("spawn_actor", {...})`` -
on purpose: ``check_tool_parity.py`` and ``tool_catalog.py`` locate the command by parsing
the tool source, so hiding it behind a variable would silently disable both checks.
"""

from __future__ import annotations

import logging
from typing import Any, Dict, Optional

logger = logging.getLogger("UnrealMCP")


def get_connection():
    """The shared bridge connection, or None. Imported lazily to avoid a circular import."""
    from unreal_mcp_server import get_unreal_connection
    return get_unreal_connection()


def call_unreal(command: str, params: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    """Send one command and normalize the reply into a tool result.

    Returns ``{"success": False, "message": ...}`` for the three cases every tool used to
    hand-roll (no connection, empty reply, raised exception). Otherwise the backend reply is
    returned unchanged, so a caller can still inspect its structured detail.
    """
    connection = get_connection()
    if not connection:
        logger.error("Failed to connect to Unreal Engine")
        return {"success": False, "message": "Failed to connect to Unreal Engine"}
    try:
        response = connection.send_command(command, params or {})
        if not response:
            logger.error("No response from Unreal Engine")
            return {"success": False, "message": "No response from Unreal Engine"}
        return response
    except Exception as error:
        message = f"{command} failed: {error}"
        logger.error(message)
        return {"success": False, "message": message}


def require_vectors3(params: Dict[str, Any], *names: str) -> Optional[str]:
    """Normalize each named parameter to a list of three floats, in place.

    Returns the first problem as a message, or None when every value is acceptable. Absent
    parameters are left alone (they are optional). Replaces the 3-float validation loop that
    each transform-taking tool used to spell out itself.
    """
    for name in names:
        value = params.get(name)
        if value is None:
            continue
        if not isinstance(value, list) or len(value) != 3:
            message = f"Invalid {name} format. Must be a list of 3 float values."
            logger.error(message)
            return message
        params[name] = [float(item) for item in value]
    return None
