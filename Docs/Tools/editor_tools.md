# Unreal MCP Editor Tools

This document provides detailed information about the editor tools available in the Unreal MCP integration.

## Overview

Editor tools let you control the Unreal Editor viewport, actors, levels and captures through MCP calls. They are particularly useful for automating tasks like placing the viewport camera, spawning and arranging actors, and capturing screenshots.

## Editor Tools

### set_viewport_camera

Place and orient the active editor viewport camera.

**Parameters:**
- `location` (array, required) - [X, Y, Z] world location for the viewport camera
- `rotation` (array, required) - [Pitch, Yaw, Roll] for the viewport camera
- `game_view` (boolean, optional) - Switch the viewport to game view

**Returns:**
- Response from Unreal Engine reporting the camera change

**Example:**
```json
{
  "command": "set_viewport_camera",
  "params": {
    "location": [0, 0, 500],
    "rotation": [0, -45, 0],
    "game_view": false
  }
}
```

### capture_viewport_screenshot

Capture the active editor viewport at its current resolution and return the saved path.

**Parameters:**
- `filename` (string, optional) - Name of the file to save the screenshot as (default: "MCP_Screenshot.png")

**Returns:**
- Result of the screenshot operation

**Example:**
```json
{
  "command": "capture_viewport_screenshot",
  "params": {
    "filename": "my_scene.png"
  }
}
```

### capture_pie_screenshot

Capture the Play-In-Editor window from a world location/orientation. Requires an active PIE session.

**Parameters:**
- `filename` (string, optional) - Output PNG name (default: "MCP_PIE_Screenshot.png")
- `location` (array, optional) - [X, Y, Z] world location for the capture camera
- `rotation` (array, optional) - [Pitch, Yaw, Roll] for the capture camera
- `width` / `height` (integer, optional) - Image size (default 1280x720)
- `fov` (number, optional) - Horizontal field of view in degrees

## Error Handling

All command responses include a "status" field indicating whether the operation succeeded, and an optional "message" field with details in case of failure.

```json
{
  "status": "error",
  "message": "Failed to get active viewport"
}
```

## Usage Examples

### Python Example

```python
from unreal_mcp_server import get_unreal_connection

# Get connection to Unreal Engine
unreal = get_unreal_connection()

# Move the viewport camera
camera_response = unreal.send_command("set_viewport_camera", {
    "location": [0, 0, 500],
    "rotation": [0, -45, 0],
    "game_view": False
})
print(camera_response)

# Capture a screenshot
screenshot_response = unreal.send_command("capture_viewport_screenshot", {"filename": "my_scene.png"})
print(screenshot_response)
```

## Troubleshooting

- **Command fails with "Failed to get active viewport"**: Make sure Unreal Editor is running and has an active viewport.
- **Invalid parameters**: Ensure that location and rotation arrays contain exactly 3 values (X, Y, Z for location; Pitch, Yaw, Roll for rotation).

## Level and console tools

### list_levels

List every map (World) asset under `/Game` with its object path.

```json
{ "command": "list_levels", "params": {} }
```

### get_current_level

Report the open level: `level_name`, `package`, `path`, and `is_dirty`.

```json
{ "command": "get_current_level", "params": {} }
```

### editor_play / editor_stop

Start or stop a Play-In-Editor session on the current map (in-process).

```json
{ "command": "editor_play", "params": {} }
{ "command": "editor_stop", "params": {} }
```

`editor_play` returns `state` `requested` (PIE starts on the next editor tick)
or `already_playing`; `editor_stop` returns `stopping` or `not_playing`. Pair
with `capture_pie_screenshot` for an automated play-capture-stop loop.

### console_command

Run an allowlisted console command. Allowed prefixes: `stat `, `show `, `r.`,
`sg.`, `foliage.`, `grass.`, `t.MaxFPS`, `HighResShot`. Anything else (for
example `quit`, `exec`, `gunit`) is refused with a clear message. Output goes to
the editor log; the reply reports whether the editor marked the command handled.

```json
{ "command": "console_command", "params": { "command": "stat fps" } }
```

## Future Enhancements

- Support for setting viewport display mode (wireframe, lit, etc.)
- Camera animation paths for cinematic viewport control
- Support for multiple viewports
