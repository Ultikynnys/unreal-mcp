"""Hot-reload the enhanced mcp_unreal_engine module inside the running Unreal instance."""
import importlib
import mcp_unreal_engine
importlib.reload(mcp_unreal_engine)
print("Reloaded mcp_unreal_engine version:", getattr(mcp_unreal_engine, "PROTOCOL_VERSION", "unknown"))
