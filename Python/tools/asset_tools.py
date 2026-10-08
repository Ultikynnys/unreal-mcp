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
from tools.mcp_client import call_unreal

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
        return call_unreal("get_job_status", {"job_id": job_id})

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
        params = {
            "path": path,
            "recursive": recursive,
            "resolve_destination": resolve_destination
        }
        return call_unreal("list_redirectors", params)

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
        params = {
            "path": path,
            "recursive": recursive,
            "delete_redirectors": delete_redirectors,
            "batch_size": batch_size
        }
        return call_unreal("fixup_redirectors", params)

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
        return call_unreal("move_assets", params)

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
        return call_unreal("move_folder", {
                "source_path": source_path,
                "destination_path": destination_path,
                "recursive": recursive,
                "dry_run": dry_run
            })

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
        params: Dict[str, Any] = {"recursive": recursive}
        if packages:
            params["packages"] = packages
        if path:
            params["path"] = path
        if "packages" not in params and "path" not in params:
            return {"success": False, "message": "Provide 'packages' or 'path'."}
        return call_unreal("resave_packages", params)

    @mcp.tool()
    def get_asset_graph(
        ctx: Context,
        asset_path: str,
        direction: str = "both",
        follow_redirectors: bool = True
    ) -> Dict[str, Any]:
        """Get an asset's dependency/referencer packages (AssetRegistry, both directions).

        direction: 'dependencies' (what this asset loads), 'referencers' (what loads
        this asset), or 'both' (default). Follows a redirector to the real asset when
        the given path is stale, so a pre-move impact check works at any cleanup stage.
        """
        return call_unreal("get_asset_graph", {
                "asset_path": asset_path,
                "direction": direction,
                "follow_redirectors": follow_redirectors,
            })

    @mcp.tool()
    def delete_assets(
        ctx: Context,
        asset_paths: List[str],
        force: bool = False
    ) -> Dict[str, Any]:
        """Delete assets and clear the redirector they leave behind.

        Each path is deleted, then the leftover source-package redirector is verified
        gone using the same machinery as move_assets cleanup. An asset that is still
        referenced is REFUSED unless force=True, and the referencing packages are named,
        because deleting a referenced asset silently nulls the reference in its consumers.
        """
        return call_unreal("delete_assets", {
                "asset_paths": asset_paths,
                "force": force,
            })

    logger.info("Asset tools registered successfully")
