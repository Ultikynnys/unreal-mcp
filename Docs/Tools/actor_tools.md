# Unreal MCP Editor Tools

This document provides detailed information about the actor tools available in the Unreal MCP integration.

## Overview

Actor tools allow you to manipulate actors in the Unreal Engine scene.

## Actor Tools

### get_actors_in_level

Get a list of all actors in the current level.

**Parameters:**
- None

**Returns:**
- List of all actors with their properties

**Example:**
```json
{
  "command": "get_actors_in_level",
  "params": {}
}
```

### spawn_actor

Create a new actor in the current level.

**Parameters:**
- `name` (string) - The name for the new actor
- `type` (string) - Actor class name; case-insensitive. One of `StaticMeshActor`,
  `PointLight`, `SpotLight`, `DirectionalLight`, `CameraActor`
- `location` (array, optional) - [X, Y, Z] coordinates for the actor's position, defaults to [0, 0, 0]
- `rotation` (array, optional) - [Pitch, Yaw, Roll] values for the actor's rotation, defaults to [0, 0, 0]
- `scale` (array, optional) - [X, Y, Z] values for the actor's scale, defaults to [1, 1, 1]

**Returns:**
- Information about the created actor

**Example:**
```json
{
  "command": "spawn_actor",
  "params": {
    "name": "MyLight",
    "type": "PointLight",
    "location": [0, 0, 100]
  }
}
```

### delete_actor

Delete an actor by name.

**Parameters:**
- `name` (string) - The name of the actor to delete

**Returns:**
- Result of the delete operation

**Example:**
```json
{
  "command": "delete_actor",
  "params": {
    "name": "MyCube"
  }
}
```

### set_actor_transform

Set the transform (location, rotation, scale) of an actor.

**Parameters:**
- `name` (string) - The name of the actor to modify
- `location` (array, optional) - [X, Y, Z] coordinates for the actor's position
- `rotation` (array, optional) - [Pitch, Yaw, Roll] values for the actor's rotation
- `scale` (array, optional) - [X, Y, Z] values for the actor's scale

**Returns:**
- Result of the transform operation

**Example:**
```json
{
  "command": "set_actor_transform",
  "params": {
    "name": "MyCube",
    "location": [100, 200, 300],
    "rotation": [0, 90, 0]
  }
}
```

### get_actor_details

Inspect an actor: world bounds, components, materials, and light settings.

**Parameters:**
- `name` (string) - The name of the actor

**Returns:**
- Object containing actor details

**Example:**
```json
{
  "command": "get_actor_details",
  "params": {
    "name": "MyCube"
  }
}
```

## Error Handling

All command responses include a "success" field indicating whether the operation succeeded, and an optional "message" field with details in case of failure.

```json
{
  "success": false,
  "message": "Actor 'MyCube' not found in the current level"
}
```

## Implementation Notes

- All numeric parameters for transforms (location, rotation, scale) must be provided as lists of 3 float values
- `spawn_actor` accepts the actor class name (case-insensitive): `StaticMeshActor`,
  `PointLight`, `SpotLight`, `DirectionalLight`, or `CameraActor`. Anything else is
  rejected with "Unknown actor type". For meshes, lights, blueprints and instanced
  meshes prefer the dedicated tools (spawn_mesh_actor, spawn_light_actor,
  spawn_blueprint_actor, spawn_mesh_grid, spawn_instanced_mesh)
- A spawned actor's name may gain a suffix to keep it unique; use the name the tool
  returns, not the one you asked for
- The server maintains logging of all operations with detailed information and error messages
- All commands are executed through a connection to the Unreal Engine editor

## Type Reference

### Actor Types

Accepted `spawn_actor` values (see Implementation Notes):

- `StaticMeshActor` - static mesh actor (assign a mesh with spawn_mesh_actor)
- `PointLight` - point light
- `SpotLight` - spot light
- `DirectionalLight` - directional light
- `CameraActor` - camera actor

## Future Extensions

The following tool categories are planned for future releases:

- **Level Tools**: Managing Unreal Engine levels
- **Material Tools**: Creating and editing materials
- **Blueprint Tools**: Manipulating Blueprints
- **Asset Tools**: Managing project assets
- **Editor Tools**: Controlling the Unreal Editor