# Unreal MCP Editor Tools

This document provides detailed information about the editor tools available in the Unreal MCP integration.

## Overview

Editor tools allow you to control the Unreal Editor viewport and other editor functionality through MCP commands. These tools are particularly useful for automating tasks like focusing the camera on specific actors or locations.

## Editor Tools

### focus_viewport

Focus the viewport on a specific actor or location.

**Parameters:**
- `target` (string, optional) - Name of the actor to focus on (if provided, location is ignored)
- `location` (array, optional) - [X, Y, Z] coordinates to focus on (used if target is None)
- `distance` (float, optional) - Distance from the target/location (default: 1000.0)
- `orientation` (array, optional) - [Pitch, Yaw, Roll] for the viewport camera

**Returns:**
- Response from Unreal Engine containing the result of the focus operation

**Example:**
```json
{
  "command": "focus_viewport",
  "params": {
    "target": "PlayerStart",
    "distance": 500,
    "orientation": [0, 180, 0]
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

# Focus on a specific actor
focus_response = unreal.send_command("focus_viewport", {
    "target": "PlayerStart",
    "distance": 500,
    "orientation": [0, 180, 0]
})
print(focus_response)

# Capture a screenshot
screenshot_response = unreal.send_command("capture_viewport_screenshot", {"filename": "my_scene.png"})
print(screenshot_response)
```

## Troubleshooting

- **Command fails with "Failed to get active viewport"**: Make sure Unreal Editor is running and has an active viewport.
- **Actor not found**: Verify that the actor name is correct and the actor exists in the current level.
- **Invalid parameters**: Ensure that location and orientation arrays contain exactly 3 values (X, Y, Z for location; Pitch, Yaw, Roll for orientation).

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
