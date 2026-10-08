#!/usr/bin/env python3
"""Live probe: a NON-map referencer must be rewritten on disk after its dependency moves.

A map referencer goes through FEditorFileUtils::SaveLevel; a content referencer (here a material
instance) goes through the package save path, which is where UEditorAssetLibrary::SaveLoadedAsset
refused with "Asset is not registered". Dev-only: leaves uniquely named content for inspection."""
import json
from pathlib import Path
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.mcp_client import call_unreal


def call(command, params):
    reply = call_unreal(command, params)
    if reply.get("status") == "error" or reply.get("success") is False:
        print(json.dumps(reply, indent=2))
        raise RuntimeError(reply)
    return reply.get("result", reply)


def job(command, params):
    queued = call(command, params)
    for _ in range(120):
        time.sleep(0.5)
        state = call("get_job_status", {"job_id": queued["job_id"]})
        print(state.get("state"), state.get("phase"), state.get("error", ""))
        if state["state"] == "failed":
            raise RuntimeError(state)
        if state["state"] == "done":
            return state
    raise TimeoutError(queued)


if __name__ == "__main__":
    call("get_capabilities", {})
    root = "/Game/MCPLiveCheck/RefSave_" + uuid.uuid4().hex[:8]
    source, destination = root + "/M_Parent", root + "/Moved/M_Parent"
    child = root + "/MIC_Child"
    call("execute_python", {"code": f'''
import unreal
root = {root!r}
tools = unreal.AssetToolsHelpers.get_asset_tools()
parent = tools.create_asset("M_Parent", root, unreal.Material, unreal.MaterialFactoryNew())
assert parent and unreal.EditorAssetLibrary.save_loaded_asset(parent, False)
child = tools.create_asset("MIC_Child", root, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
unreal.MaterialEditingLibrary.set_material_instance_parent(child, parent)
assert unreal.EditorAssetLibrary.save_loaded_asset(child, False)
print("content referencer saved")
'''})
    job("move_assets", {"moves": [{"source": source, "destination": destination}], "fixup_redirectors": True})
    call("execute_python", {"code": f'''
import unreal
from pathlib import Path
content = Path(unreal.Paths.project_content_dir())
old, new, child = {source!r}, {destination!r}, {child!r}
name = child.removeprefix("/Game/") + ".uasset"
data = (content / name).read_bytes()
encs = ("utf-8", "utf-16-le")
assert not any(old.encode(e) in data for e in encs), "old parent path still on the referencer disk"
assert any(new.encode(e) in data for e in encs), "new parent path absent on the referencer disk"
assert unreal.EditorAssetLibrary.does_asset_exist(new)
assert not unreal.EditorAssetLibrary.does_asset_exist(old)
print("PASS: content referencer imports the new path, old path absent")
'''})
    print("Probe content retained:", root)
