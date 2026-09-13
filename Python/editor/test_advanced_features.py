"""
Integration test for newly added Unreal MCP commands.
"""
import socket
import json

def send(cmd, params=None):
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

print("1. Testing get_capabilities...")
caps = send("get_capabilities")
print("Version:", caps.get("result", {}).get("version"))
assert caps.get("result", {}).get("version") == "1.2.0"

print("2. Testing spawn_mesh_grid...")
grid = send("spawn_mesh_grid", {
    "mesh_path": "/Game/Art/Mesh/Props/LevelLibrary_Assets/SM_Floor.SM_Floor",
    "rows": 2,
    "cols": 3,
    "spacing_x": 200,
    "spacing_y": 200,
    "origin": [5000, 5000, 0],
    "prefix": "TestFloorGrid",
    "folder_path": "Tests/Grids"
})
print("Spawned grid actors:", grid.get("result", {}).get("count"))
assert grid.get("result", {}).get("count") == 6

print("3. Testing spawn_light_actor...")
light = send("spawn_light_actor", {
    "light_type": "PointLight",
    "name": "TestGoldenSconce",
    "location": [5100, 5100, 200],
    "intensity": 5000.0,
    "color": [1.0, 0.7, 0.2],
    "attenuation_radius": 800.0,
    "folder_path": "Tests/Lighting"
})
print("Spawned light:", light.get("result", {}).get("label"), light.get("result", {}).get("folder"))
assert light.get("result", {}).get("label") == "TestGoldenSconce"
assert light.get("result", {}).get("folder") == "Tests/Lighting"

print("4. Testing set_actor_folder...")
folder_res = send("set_actor_folder", {
    "name": "TestGoldenSconce",
    "folder_path": "Tests/ReorganizedLighting"
})
print("Updated folder:", folder_res.get("result", {}).get("folder_path"))
assert folder_res.get("result", {}).get("folder_path") == "Tests/ReorganizedLighting"

print("\nAll integration tests passed successfully!")
