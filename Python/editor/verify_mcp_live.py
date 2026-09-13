"""
Test the updated backend endpoints directly against port 55557
"""
import socket
import json

def call_unreal(cmd, params=None):
    s = socket.socket()
    s.connect(('127.0.0.1', 55557))
    payload = json.dumps({"type": cmd, "params": params or {}}).encode()
    s.sendall(payload)
    data = bytearray()
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        data.extend(chunk)
        try:
            return json.loads(data)
        except:
            continue

print("=== 1. Capability Handshake ===")
caps = call_unreal("get_capabilities")
print("Response:", json.dumps(caps, indent=2))

print("\n=== 2. Create Level ===")
created = call_unreal("create_level", {
    "map_path": "/Game/Maps/TestLab_MCP",
    "overwrite": True
})
print("Create level response:", json.dumps(created, indent=2))

print("\n=== 3. Spawn Mesh Grid (Flooring) ===")
grid = call_unreal("spawn_mesh_grid", {
    "mesh_path": "/Game/Art/Mesh/Props/LevelLibrary_Assets/SM_Floor.SM_Floor",
    "rows": 4,
    "cols": 4,
    "spacing_x": 200.0,
    "spacing_y": 200.0,
    "origin": [0.0, 0.0, 0.0],
    "prefix": "LabFloor",
    "folder_path": "TestLab/Floors"
})
print("Grid spawn response:", json.dumps(grid, indent=2))

print("\n=== 4. Spawn First-Class Light Actor ===")
light = call_unreal("spawn_light_actor", {
    "light_type": "PointLight",
    "name": "Lab_CenterLight",
    "location": [300.0, 300.0, 350.0],
    "intensity": 6500.0,
    "color": [1.0, 0.75, 0.3],
    "attenuation_radius": 1200.0,
    "source_radius": 30.0,
    "mobility": "movable",
    "folder_path": "TestLab/Lighting"
})
print("Light spawn response:", json.dumps(light, indent=2))

print("\n=== 5. Reorganize Folder with set_actor_folder ===")
folder_change = call_unreal("set_actor_folder", {
    "name": "Lab_CenterLight",
    "folder_path": "TestLab/Lighting/Primary"
})
print("Folder update response:", json.dumps(folder_change, indent=2))

print("\n=== 6. Material Assignment with set_actor_material ===")
mat_change = call_unreal("set_actor_material", {
    "name": "LabFloor_00_00",
    "slot_index": 0,
    "material_path": "/Game/Art/Mesh/Props/LevelLibrary_Assets/MI_MetalFloor.MI_MetalFloor"
})
if mat_change.get("status") != "success":
    # Try another material from the project
    mat_change = call_unreal("set_actor_material", {
        "name": "LabFloor_00_00",
        "slot_index": 0,
        "material_path": "/Game/Art/Materials/M_BaseFloor.M_BaseFloor"
    })
print("Material assignment response:", json.dumps(mat_change, indent=2))

print("\n=== 7. Instanced Mesh Spawning (HISM) ===")
# Spawn 20 book instances packed into 1 actor
book_instances = []
for i in range(20):
    book_instances.append({
        "location": [100.0, 50.0 + (i * 25.0), 50.0],
        "rotation": [0.0, 90.0, 0.0],
        "scale": [1.0, 1.0, 1.0]
    })

instanced = call_unreal("spawn_instanced_mesh", {
    "mesh_path": "/Game/Art/Mesh/Props/LevelLibrary_Assets/SM_Book.SM_Book",
    "name": "Lab_BookStack_HISM",
    "instances": book_instances,
    "folder_path": "TestLab/Props"
})
print("Instanced mesh response:", json.dumps(instanced, indent=2))

print("\n=== 8. Save Level ===")
saved = call_unreal("save_level", {})
print("Save level response:", json.dumps(saved, indent=2))
