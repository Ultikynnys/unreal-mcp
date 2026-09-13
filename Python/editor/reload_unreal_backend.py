"""Reload mcp_unreal_engine directly inside the running Unreal instance."""
import importlib
import mcp_unreal_engine
importlib.reload(mcp_unreal_engine)
print("Reloaded protocol version:", getattr(mcp_unreal_engine, "PROTOCOL_VERSION", "unknown"))
