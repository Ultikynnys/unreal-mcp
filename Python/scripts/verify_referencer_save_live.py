#!/usr/bin/env python3
"""Dev-only persistence probe. Leaves its uniquely named content for inspection."""
import json
from pathlib import Path
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.mcp_client import call_unreal


def call(command, params):
    reply = call_unreal(command, params)
    print(json.dumps(reply, indent=2))
    if reply.get("status") == "error" or reply.get("success") is False:
        raise RuntimeError(reply)
    return reply.get("result", reply)


def job(command, params):
    queued = call(command, params)
    for _ in range(120):
        time.sleep(0.5)
        state = call("get_job_status", {"job_id": queued["job_id"]})
        if state["state"] == "failed":
            raise RuntimeError(state)
        if state["state"] == "done":
            return state
    raise TimeoutError(queued)


if __name__ == "__main__":
    call("get_capabilities", {})
    if len(sys.argv) > 1:
        state = call("get_job_status", {"job_id": sys.argv[2]})
        assert state["state"] == "done", state
    root = sys.argv[1] if len(sys.argv) > 1 else "/Game/MCPLiveCheck/ForcedSave_" + uuid.uuid4().hex[:8]
    source, destination, level = root + "/M_Source", root + "/Moved/M_Target", root + "/L_Ref"
    call("execute_python", {"code": f'''
import unreal
root = {root!r}
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
material = asset_tools.create_asset("M_Source", root, unreal.Material, unreal.MaterialFactoryNew())
assert material
assert unreal.EditorAssetLibrary.save_loaded_asset(material, False)
levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
assert levels.new_level({level!r})
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
actor = actors.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(0, 0, 0))
component = actor.static_mesh_component
component.set_static_mesh(unreal.load_asset("/Engine/BasicShapes/Cube"))
component.set_material(0, material)
assert levels.save_current_level()
print("clean level saved before rename")
''' if len(sys.argv) == 1 else 'print("resuming existing probe")'})
    if len(sys.argv) == 1:
        job("move_assets", {"moves": [{"source": source, "destination": destination}], "fixup_redirectors": True})
    job("resave_packages", {"packages": [level]})
    call("execute_python", {"code": f'''
import unreal
from pathlib import Path
filename = Path(unreal.Paths.project_content_dir()) / { (level.removeprefix('/Game/') + '.umap')!r}
data = filename.read_bytes()
old = {source!r}
new = {destination!r}
assert old.encode() not in data and old.encode("utf-16-le") not in data, "old path remains on disk"
assert new.encode() in data or new.encode("utf-16-le") in data, "new path absent on disk"
assert unreal.EditorAssetLibrary.does_asset_exist(new)
assert not unreal.EditorAssetLibrary.does_asset_exist(old)
print("PASS: saved map contains new path, old path absent; native disk import verification passed")
'''})
    print("Probe content retained:", root)
