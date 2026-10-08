"""Live integration test using the registered MCP tools and their server connection."""
import argparse
import json
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import unreal_mcp_server
from tools.asset_tools import register_asset_tools
from tools.editor_tools import register_editor_tools


class Registry:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def register(function):
            self.tools[function.__name__] = function
            return function
        return register


registry = Registry()
register_asset_tools(registry)
register_editor_tools(registry)
parser = argparse.ArgumentParser()
parser.add_argument("mode", choices=["setup", "stress", "stress2", "stress3", "cold", "verify", "quit"])
parser.add_argument("--root", default="/Game/MCPRestructureStress_20261008_R3")
args = parser.parse_args()
root = args.root
report_path = Path(__file__).with_name(root.rsplit('/', 1)[-1] + "_report.json")
report = json.loads(report_path.read_text()) if report_path.exists() else {"root": root, "events": []}


def record(name, result):
    report["events"].append({"name": name, "result": result})
    report_path.write_text(json.dumps(report, indent=2))
    print(name, json.dumps({k: v for k, v in result.items() if k not in ('moves', 'items')}), flush=True)


def call(name, **params):
    result = registry.tools[name](None, **params)
    if result.get("success") is False:
        raise AssertionError(result)
    return result.get("result", result)


def py(code):
    result = call("execute_python", code=code)
    if result.get("success") is False:
        raise AssertionError(result)
    return result


def job(name, **params):
    result = call(name, **params)
    record(name + " start", result)
    deadline = time.monotonic() + 900
    while time.monotonic() < deadline:
        try:
            status = call("get_job_status", job_id=result["job_id"])
        except AssertionError as error:
            record("status transport failure", {"error": str(error)})
            time.sleep(1)
            continue
        if status["state"] in ("done", "failed"):
            record(name + " terminal", status)
            assert status["state"] == "done", status
            assert status["done"] == status["total"], status
            return status
        time.sleep(0.5)
    raise TimeoutError(result)


def verify(prefix, label):
    code = f'''
import unreal, json
root = {root!r}
prefix = {prefix!r}
el = unreal.EditorAssetLibrary
ar = unreal.AssetRegistryHelpers.get_asset_registry()
checked = 0
for i in range(12):
    branch = 'Region_%02d/Props/Shared/Deep' % (i % 4)
    base = prefix + '/' + branch
    mat = el.load_asset(base + '/Materials/M_%02d' % i)
    mi = el.load_asset(base + '/Instances/MI_%02d' % i)
    mesh = el.load_asset(base + '/Meshes/SM_%02d' % i)
    outside = el.load_asset(root + '/Outside/MI_Link_%02d' % i)
    assert mat and mi and mesh and outside, ('missing', i, base)
    assert mi.get_editor_property('parent') == mat, ('parent', i)
    assert outside.get_editor_property('parent') == mi, ('outside', i)
    assert mesh.get_material(0) == mi, ('mesh material', i)
    bp = el.load_asset(base + '/Blueprints/BP_Prop_%02d' % i)
    assert bp, ('missing blueprint', i)
    gen = unreal.load_object(None, bp.get_path_name() + '_C')
    assert gen, ('no generated class', i)
    cdo = unreal.get_default_object(gen)
    comp = cdo.get_editor_property('root_component') if False else None
    comps = [c for c in cdo.get_components_by_class(unreal.StaticMeshComponent)]
    assert comps, ('no components', i)
    assert comps[0].get_editor_property('static_mesh') == mesh, ('bp mesh', i)
    assert comps[0].get_material(0) == mi, ('bp material', i)
    checked += 6
redirectors = [str(a.package_name) for a in ar.get_assets_by_path(root, recursive=True) if str(a.asset_class_path.asset_name) == 'ObjectRedirector']
assert not redirectors, redirectors
print(json.dumps({{'checked_reference_objects': checked, 'redirectors': redirectors, 'label': {label!r}}}))
'''
    record(label, py(code))


try:
    if args.mode == "setup":
        record("setup", py(f'''
import unreal, json
root = {root!r}
el = unreal.EditorAssetLibrary
assert not el.does_directory_exist(root), 'Test root already exists'
at = unreal.AssetToolsHelpers.get_asset_tools()
for i in range(12):
    base = root + '/Source/Region_%02d/Props/Shared/Deep' % (i % 4)
    mat = at.create_asset('M_%02d' % i, base + '/Materials', unreal.Material, unreal.MaterialFactoryNew())
    mi = at.create_asset('MI_%02d' % i, base + '/Instances', unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    unreal.MaterialEditingLibrary.set_material_instance_parent(mi, mat)
    mesh = el.duplicate_asset('/Engine/BasicShapes/Cube', base + '/Meshes/SM_%02d' % i)
    mesh.set_material(0, mi)
    outside = at.create_asset('MI_Link_%02d' % i, root + '/Outside', unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    unreal.MaterialEditingLibrary.set_material_instance_parent(outside, mi)
    for asset in [mat, mi, mesh, outside]:
        assert el.save_loaded_asset(asset, only_if_is_dirty=False)
    factory = unreal.BlueprintFactory()
    factory.set_editor_property('parent_class', unreal.StaticMeshActor)
    bp = at.create_asset('BP_Prop_%02d' % i, base + '/Blueprints', unreal.Blueprint, factory)
    gen = unreal.load_object(None, bp.get_path_name() + '_C')
    assert gen, ('no generated class', i)
    cdo = unreal.get_default_object(gen)
    comps = cdo.get_components_by_class(unreal.StaticMeshComponent)
    assert comps, ('no native component template', i)
    comps[0].set_static_mesh(mesh)
    comps[0].set_material(0, mi)
    unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    assert el.save_loaded_asset(bp, only_if_is_dirty=False)
for branch in ['BranchA', 'BranchB']:
    mat = at.create_asset('SameName', root + '/Source/' + branch + '/Nested', unreal.Material, unreal.MaterialFactoryNew())
    assert el.save_loaded_asset(mat, only_if_is_dirty=False)
for path in ['/Shallow/Top', '/Shallow/Nested/Keep', '/StageA/Art/Existing/Sentinel']:
    folder, name = (root + path).rsplit('/', 1)
    asset = at.create_asset(name, folder, unreal.Material, unreal.MaterialFactoryNew())
    assert el.save_loaded_asset(asset, only_if_is_dirty=False)
world = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).new_level(root + '/Outside/L_References')
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
for i in range(12):
    mesh = el.load_asset(root + '/Source/Region_%02d/Props/Shared/Deep/Meshes/SM_%02d' % (i % 4, i))
    actor = actors.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(i * 150, 0, 0))
    actor.static_mesh_component.set_static_mesh(mesh)
bpgen = unreal.load_object(None, el.load_asset(root + '/Source/Region_00/Props/Shared/Deep/Blueprints/BP_Prop_00').get_path_name() + '_C')
bpmesh = el.load_asset(root + '/Source/Region_00/Props/Shared/Deep/Meshes/SM_00')
spawned = actors.spawn_actor_from_class(bpgen, unreal.Vector(0, 400, 0))
assert spawned, 'blueprint actor failed to spawn'
assert unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level()
print(json.dumps({{'assets': 66, 'blueprints': 12, 'meshes': 12, 'cross_folder_referencers': 12, 'level_actors': 13}}))
'''))
        verify(root + "/Source", "initial references")
    elif args.mode == "stress":
        for name, params in [
            ("nested", {"source_path": root + "/Source", "destination_path": root + "/Source/Nested"}),
            ("self", {"source_path": root + "/Source", "destination_path": root + "/Source"}),
        ]:
            result = registry.tools["move_folder"](None, dry_run=True, **params)
            assert result.get("success") is False, result
            record("rejected " + name, result)
        collision = registry.tools["move_assets"](None, moves=[{"source": root + "/Source/BranchA/Nested/SameName", "destination": root + "/Source/BranchB/Nested/SameName"}], dry_run=True)
        assert collision.get("success") is False, collision
        record("rejected collision", collision)
        malformed = registry.tools["move_assets"](None, moves=[{"source": "not/a/package", "destination": root + "/Bad/Foo"}], dry_run=True)
        assert malformed.get("success") is False, malformed
        record("rejected malformed", malformed)
        preview = call("move_folder", source_path=root + "/Source", destination_path=root + "/StageA/Art", dry_run=True)
        assert preview["count"] == 50, preview
        record("recursive preview", preview)
        verify(root + "/Source", "negative cases and preview did not mutate")
        shallow = registry.tools["move_folder"](None, source_path=root + "/Source", destination_path=root + "/Shallow", recursive=False, dry_run=True)
        assert shallow.get("success") is False, shallow
        record("nonrecursive excludes nested assets", shallow)
        job("move_folder", source_path=root + "/Shallow", destination_path=root + "/ShallowMoved", recursive=False)
        record("shallow success", py(f"import unreal\nassert unreal.EditorAssetLibrary.does_asset_exist('{root}/ShallowMoved/Top')\nassert unreal.EditorAssetLibrary.does_asset_exist('{root}/Shallow/Nested/Keep')"))
        current = root + "/Source"
        for destination in [root + "/StageA/Art", root + "/StageB/Library/Assets", root + "/StageC/Deep/Organized", root + "/Source"]:
            job("move_folder", source_path=current, destination_path=destination)
            current = destination
            verify(current, "verify " + current)
        source = root + "/Source/BranchA/Nested/SameName"
        for destination in [root + "/Chains/One/SameName", root + "/Chains/Two/Renamed"]:
            job("move_assets", moves=[{"source": source, "destination": destination}], fixup_redirectors=False)
            source = destination
        job("move_assets", moves=[{"source": source, "destination": root + "/Chains/Three/Final"}])
        inventory = call("list_redirectors", path=root, recursive=True, resolve_destination=True)
        record("chain redirectors after default final move", inventory)
        assert inventory["count"] == 0, inventory
        record("save level", py("import unreal\nassert unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level()"))
        report["stress_passed"] = True
        report_path.write_text(json.dumps(report, indent=2))
    elif args.mode == "stress3":
        # Resume path after Source -> StageA/Art already done and verified.
        current = root + "/StageA/Art"
        for destination in [root + "/StageB/Library/Assets", root + "/StageC/Deep/Organized", root + "/Source"]:
            job("move_folder", source_path=current, destination_path=destination)
            current = destination
            verify(current, "verify " + current)
        source = root + "/Source/BranchA/Nested/SameName"
        for destination in [root + "/Chains/One/SameName", root + "/Chains/Two/Renamed"]:
            job("move_assets", moves=[{"source": source, "destination": destination}], fixup_redirectors=False)
            source = destination
        job("move_assets", moves=[{"source": source, "destination": root + "/Chains/Three/Final"}])
        inventory = call("list_redirectors", path=root, recursive=True, resolve_destination=True)
        record("chain redirectors after default final move", inventory)
        assert inventory["count"] == 0, inventory
        record("save level", py("import unreal\nassert unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level()"))
        report["stress_passed"] = True
        report_path.write_text(json.dumps(report, indent=2))
    elif args.mode == "verify":
        verify(root + "/Source", "standalone verify")
    elif args.mode == "cold":
        record("cold level actor references", py(f'''
import unreal, json
root = {root!r}
el = unreal.EditorAssetLibrary
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
out = {{'static_mesh_actors': 0, 'bp_actors': 0, 'stale': []}}
for a in actors:
    if isinstance(a, unreal.StaticMeshActor):
        out['static_mesh_actors'] += 1
        m = a.static_mesh_component.get_editor_property('static_mesh')
        assert m, 'actor missing mesh'
        if 'MCPRestructureStress' in m.get_path_name() and not m.get_path_name().startswith(root + '/Source/'):
            out['stale'].append(m.get_path_name())
    elif a.get_class().get_name().startswith('BP_Prop_'):
        out['bp_actors'] += 1
        for c in a.get_components_by_class(unreal.StaticMeshComponent):
            m = c.get_editor_property('static_mesh')
            assert m, 'bp actor missing mesh'
            if not m.get_path_name().startswith(root + '/Source/'):
                out['stale'].append(m.get_path_name())
assert not out['stale'], out['stale']
print(json.dumps(out))
'''))
        verify(root + "/Source", "cold loaded references")
        record("cold level", py(f'''
import unreal, json
root = {root!r}
assert unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(root + '/Outside/L_References')
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
# The BP actor derives from StaticMeshActor, so isinstance() matches it too:
# separate by exact class name instead.
meshes = [a.static_mesh_component.get_editor_property('static_mesh') for a in actors if a.get_class().get_name() == 'StaticMeshActor']
bp_actors = [a for a in actors if a.get_class().get_name().startswith('BP_Prop_')]
assert len(meshes) == 12, len(meshes)
assert all(m and m.get_path_name().startswith(root + '/Source/') for m in meshes)
assert len(bp_actors) == 1, len(bp_actors)
for a in bp_actors:
    for c in a.get_components_by_class(unreal.StaticMeshComponent):
        m = c.get_editor_property('static_mesh')
        assert m and m.get_path_name().startswith(root + '/Source/'), m.get_path_name() if m else None
print(json.dumps({{'persisted_level_mesh_references': len(meshes), 'persisted_bp_actors': len(bp_actors)}}))
'''))
        report["cold_passed"] = True
        report_path.write_text(json.dumps(report, indent=2))
    elif args.mode == "quit":
        record("graceful quit requested", py("import unreal\nunreal.SystemLibrary.quit_editor()"))
except Exception as error:
    record(args.mode + " FAILED", {"error": str(error)})
    raise
