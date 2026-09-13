"""Prove reload_server picks up on-disk changes without an editor restart."""
import json
import socket
from pathlib import Path

TARGET = Path('C:/Users/uraan/Documents/gmtk2026/Content/Python/mcp_unreal_engine.py')

def call(cmd, params=None):
    s = socket.socket()
    s.connect(('127.0.0.1', 55557))
    s.sendall(json.dumps({"type": cmd, "params": params or {}}).encode())
    data = bytearray()
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        data.extend(chunk)
        try:
            return json.loads(data)
        except Exception:
            continue

original = TARGET.read_text(encoding='utf-8')
assert 'PROTOCOL_VERSION = "1.3.0"' in original

try:
    print("before reload:", call("get_capabilities")["result"]["version"])

    # Change the on-disk source and hot-reload (NO editor restart).
    TARGET.write_text(original.replace('PROTOCOL_VERSION = "1.3.0"', 'PROTOCOL_VERSION = "1.3.1"'), encoding='utf-8')
    rl = call("reload_server")
    print("reload:", rl["status"], rl.get("result", {}).get("version"))
    after = call("get_capabilities")["result"]["version"]
    print("after reload (expect 1.3.1):", after)
    assert after == "1.3.1", f"hot reload did not pick up change: {after}"
    print("HOT RELOAD CONFIRMED: on-disk change applied with no editor restart")
finally:
    # Restore original source and reload back to 1.3.0.
    TARGET.write_text(original, encoding='utf-8')
    call("reload_server")
    print("restored version:", call("get_capabilities")["result"]["version"])
