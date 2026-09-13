"""Verify all five fixes through the MCP tool layer (also exercises the canonical envelope)."""
import asyncio
import sys
import time
sys.path.insert(0, r'C:\Users\uraan\.reasonix\tools\unreal-mcp\Python')

import unreal_mcp_server as srv
tm = srv.mcp._tool_manager

async def run(name, args=None):
    r = await tm.call_tool(name, args or {})
    # Canonical envelope: every reply has success+message; success also has result.
    assert isinstance(r, dict), f"{name} returned non-dict"
    assert {"success", "message"} <= set(r.keys()), f"{name} envelope keys: {r.keys()}"
    if r.get("success") is True:
        assert "result" in r, f"{name} success reply missing result"
    return r

async def main():
    print("== 1. capabilities are registry-derived (v1.3.0 + new cmds) ==")
    caps = await run("get_capabilities")
    info = caps["result"]
    print("version:", info["version"], "| command count:", len(info["supported_commands"]))
    for c in ["reload_server", "delete_level", "delete_actors_by_prefix"]:
        assert c in info["supported_commands"], f"{c} missing from handshake"
    print("handshake includes reload_server/delete_level/delete_actors_by_prefix: OK")

    print("\n== 2. duplicate-label guard ==")
    await run("create_level", {"map_path": f"/Game/Maps/FixAll_{int(time.time())}", "overwrite": True})
    a = await run("spawn_actor", {"name": "DupTarget", "type": "PointLight", "location": [0, 0, 100]})
    print("first spawn success:", a["success"])
    b = await run("spawn_actor", {"name": "DupTarget", "type": "PointLight", "location": [0, 0, 100]})
    print("dup spawn success:", b["success"], "| message:", b["message"])
    assert a["success"] is True
    assert b["success"] is False and "already in use" in b["message"]
    c = await run("spawn_actor", {"name": "DupTarget", "type": "PointLight", "location": [0, 0, 100], "allow_duplicate": True})
    print("allow_duplicate override success:", c["success"])
    assert c["success"] is True

    print("\n== 3. bulk cleanup by prefix ==")
    for i in range(5):
        await run("spawn_actor", {"name": f"CleanupTest_{i}", "type": "PointLight", "location": [i * 100, 0, 100]})
    cl = await run("delete_actors_by_prefix", {"prefix": "CleanupTest_"})
    print("deleted_count:", cl["result"]["deleted_count"])
    assert cl["success"] and cl["result"]["deleted_count"] == 5

    print("\n== 4. delete_level (refuse-if-open) ==")
    dts = int(time.time())
    await run("create_level", {"map_path": f"/Game/Maps/DeleteA_{dts}", "overwrite": True})
    await run("create_level", {"map_path": f"/Game/Maps/DeleteB_{dts}", "overwrite": True})
    d1 = await run("delete_level", {"map_path": f"/Game/Maps/DeleteA_{dts}"})       # not open -> success
    print("delete non-open level success:", d1["success"])
    d2 = await run("delete_level", {"map_path": f"/Game/Maps/DeleteB_{dts}"})       # open -> refuse
    print("delete open level success:", d2["success"], "| message:", d2["message"])
    assert d1["success"] is True
    assert d2["success"] is False and "currently open" in d2["message"]

    print("\n== 5. reload_server ==")
    rl = await run("reload_server")
    print("reload success:", rl["success"], "| version:", rl["result"].get("version") if rl["success"] else rl["message"])
    assert rl["success"] is True and rl["result"]["version"] == "1.3.0"
    # still functional after hot reload
    after = await run("get_capabilities")
    print("post-reload handshake OK:", after["success"])
    assert after["success"] is True

    print("\nALL FIVE FIXES VERIFIED THROUGH THE MCP LAYER")

asyncio.run(main())
