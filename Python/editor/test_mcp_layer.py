"""Exercise the registered FastMCP tools end-to-end (not raw sockets)."""
import asyncio
import sys
sys.path.insert(0, r'C:\Users\uraan\.reasonix\tools\unreal-mcp\Python')

import unreal_mcp_server as srv

async def main():
    tm = srv.mcp._tool_manager

    async def run(name, args):
        return await tm.call_tool(name, args)

    print("== MCP-layer: get_capabilities ==")
    caps = await run("get_capabilities", {})
    print(str(caps)[:200])

    print("\n== MCP-layer: create_level ==")
    cl = await run("create_level", {"map_path": "/Game/Maps/McpLayerVerify", "overwrite": True})
    print(cl)

    print("\n== MCP-layer: spawn_instanced_mesh (12 instances) ==")
    inst = await run("spawn_instanced_mesh", {
        "mesh_path": "/Game/Art/Mesh/Props/LevelLibrary_Assets/SM_Book.SM_Book",
        "name": "McpLayerHISM",
        "instances": [{"location": [i * 40.0, 0, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1]} for i in range(12)]
    })
    print(inst)

    print("\n== MCP-layer: spawn_light_actor ==")
    lt = await run("spawn_light_actor", {
        "light_type": "PointLight", "name": "McpLayerLight",
        "location": [0, 0, 300], "intensity": 5000.0, "color": [1.0, 0.7, 0.3], "source_radius": 30.0
    })
    print(str(lt)[:260])

    print("\n== MCP-layer: save_level ==")
    sv = await run("save_level", {})
    print(sv)

asyncio.run(main())
