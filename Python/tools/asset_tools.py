"""
Asset organization tools for Unreal MCP.

First-class primitives for moving/renaming assets and clearing the
ObjectRedirectors UE leaves behind - the "sorting" operations an agent would
otherwise have to script through execute_python (slow, blind, and unable to fix
references). move_assets / move_folder / fixup_redirectors / resave_packages run
as asynchronous jobs polled with get_job_status. Native rename/fixup calls can
still block the editor thread for the duration of an individual operation.

Note: the project rule in .cursor/rules/tools.mdc applies here (no Any/object/
Optional/Union parameter types; defaults written as `x: T = None` and resolved in
the body).
"""

import logging
from typing import Dict, List, Any
from mcp.server.fastmcp import FastMCP, Context

# Get logger
logger = logging.getLogger("UnrealMCP")


def register_asset_tools(mcp: FastMCP):
    """Register asset organization tools with the MCP server."""

    @mcp.tool()
    def get_job_status(ctx: Context, job_id: str) -> Dict[str, Any]:
        """Poll an asset job started by move_assets / move_folder / fixup_redirectors / resave_packages.

        Returns {job_id, kind, state, phase, done, total, error, items, log}. state is one
        of queued | running | done | failed, and done/total is the progress counter.

        Example: get_job_status(job_id="1f3a9c...")
        """
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            return unreal.send_command("get_job_status", {"job_id": job_id})
        except Exception as e:
            logger.error(f"Error polling job: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def list_redirectors(
        ctx: Context,
        path: str = "/Game",
        recursive: bool = True,
        resolve_destination: bool = False
    ) -> Dict[str, Any]:
        """List the ObjectRedirectors under a path - clutter left behind after assets move.

        Each entry reports referencer_count (how many packages still resolve through the old
        path) and, when resolve_destination is True, the destination it now points at.

        Example: list_redirectors(path="/Game/Art", recursive=True)
        """
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            params = {
                "path": path,
                "recursive": recursive,
                "resolve_destination": resolve_destination
            }
            return unreal.send_command("list_redirectors", params)
        except Exception as e:
            logger.error(f"Error listing redirectors: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def fixup_redirectors(
        ctx: Context,
        path: str = "/Game",
        recursive: bool = True,
        delete_redirectors: bool = True,
        batch_size: int = 1
    ) -> Dict[str, Any]:
        """Fix up references to every redirector under a path and (optionally) delete them.

        This is the scriptable "Fix Up Redirectors in Folder": it resaves the packages still
        pointing at the old paths and, when delete_redirectors is True, removes the
        redirectors. Runs asynchronously - returns a job_id; poll get_job_status.

        Example: fixup_redirectors(path="/Game/Art", recursive=True)
        """
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            params = {
                "path": path,
                "recursive": recursive,
                "delete_redirectors": delete_redirectors,
                "batch_size": batch_size
            }
            return unreal.send_command("fixup_redirectors", params)
        except Exception as e:
            logger.error(f"Error fixing up redirectors: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def move_assets(
        ctx: Context,
        moves: List[Dict[str, Any]] = None,
        assets: List[str] = None,
        destination_path: str = None,
        dry_run: bool = False,
        fixup_redirectors: bool = True
    ) -> Dict[str, Any]:
        """Move/rename assets, saving and fixing source redirectors after each move.

        Preflights the whole batch. dry_run returns the validated mapping without changes.
        Otherwise returns a job_id; poll get_job_status until done or failed.
        Stops on the first rename/save/cleanup failure; earlier moves are not rolled back.

        Provide either:
        - moves: [{"source": "/Game/Old/Foo", "destination": "/Game/New/Foo"}, ...], or
        - assets: ["/Game/Old/Foo", ...] together with destination_path (each keeps its name).

        Cleanup is automatic unless fixup_redirectors=False. Never move .uasset files on disk.

        Example: move_assets(assets=["/Game/Weapons/Pistol/Pistol_01"], destination_path="/Game/Art/Weapons/Pistol")
        """
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            if moves is not None and (assets is not None or destination_path is not None):
                return {"success": False, "message": "Provide only moves, or assets with destination_path."}
            params: Dict[str, Any] = {"dry_run": dry_run, "fixup_redirectors": fixup_redirectors}
            if moves:
                params["moves"] = moves
            if assets:
                params["assets"] = assets
            if destination_path:
                params["destination_path"] = destination_path
            if "moves" not in params and not ("assets" in params and "destination_path" in params):
                return {"success": False, "message": "Provide 'moves', or 'assets' with 'destination_path'."}
            return unreal.send_command("move_assets", params)
        except Exception as e:
            logger.error(f"Error moving assets: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def move_folder(
        ctx: Context,
        source_path: str,
        destination_path: str,
        recursive: bool = True,
        dry_run: bool = False
    ) -> Dict[str, Any]:
        """Move folder contents preserving subfolders, fixing redirectors after each asset.

        Use /Game paths, not filesystem paths. dry_run previews all moves and collisions.
        Excludes existing redirectors; never deletes source folders or overwrites assets.
        Returns a job_id (kind move_assets); poll get_job_status until done or failed.
        Stops on cleanup failure without rolling back earlier moves. Nested folders are rejected.
        """
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            return unreal.send_command("move_folder", {
                "source_path": source_path,
                "destination_path": destination_path,
                "recursive": recursive,
                "dry_run": dry_run
            })
        except Exception as e:
            logger.error(f"Error moving folder: {e}")
            return {"success": False, "message": str(e)}

    @mcp.tool()
    def resave_packages(
        ctx: Context,
        packages: List[str] = None,
        path: str = None,
        recursive: bool = True
    ) -> Dict[str, Any]:
        """Load and save packages. Runs asynchronously - returns a job_id; poll get_job_status.

        Provide an explicit packages list, or a path to resave every package under it.
        Resaving resolves redirector imports and writes the new paths - the scriptable
        substitute for "load + resave every referencing package" before deleting redirectors.

        Example: resave_packages(path="/Game/Maps", recursive=True)
        """
        from unreal_mcp_server import get_unreal_connection
        try:
            unreal = get_unreal_connection()
            if not unreal:
                return {"success": False, "message": "Failed to connect to Unreal Engine"}
            params: Dict[str, Any] = {"recursive": recursive}
            if packages:
                params["packages"] = packages
            if path:
                params["path"] = path
            if "packages" not in params and "path" not in params:
                return {"success": False, "message": "Provide 'packages' or 'path'."}
            return unreal.send_command("resave_packages", params)
        except Exception as e:
            logger.error(f"Error resaving packages: {e}")
            return {"success": False, "message": str(e)}

    logger.info("Asset tools registered successfully")
