"""Send a Python script to the project's running local Unreal MCP server."""
import json
from pathlib import Path
import socket
import sys

code = Path(sys.argv[1]).read_text(encoding="utf-8")
with socket.create_connection(("127.0.0.1", 55557), timeout=10) as connection:
    connection.settimeout(120)
    connection.sendall(json.dumps({"type": "execute_python", "params": {"code": code}}).encode())
    data = bytearray()
    while True:
        chunk = connection.recv(65536)
        if not chunk:
            break
        data.extend(chunk)
response = json.loads(data)
print(json.dumps(response, indent=2))
if response.get("status") != "success":
    sys.exit(1)
