# Unreal MCP Asset Tools

Tools for organizing assets: moving/renaming them and cleaning up the
ObjectRedirectors UE leaves behind (the "sorting" operations that otherwise had
to be scripted through `execute_python`).

## Long-running jobs

`move_assets`, `fixup_redirectors`, and `resave_packages` run as **asynchronous
jobs** on the Unreal side and return a `job_id` immediately. Poll that id with
`get_job_status` until `state` is `done` or `failed`. This keeps the editor (and
the MCP socket) responsive, so a multi-minute operation never trips a read
timeout.

## Asset Tools

### list_redirectors

List the ObjectRedirectors under a path, with how many packages still resolve
through each old path.

**Parameters:**
- `path` (string) - Root folder (default: `/Game`)
- `recursive` (bool) - Recurse into subfolders (default: `true`)
- `resolve_destination` (bool) - Load each redirector to report its destination (default: `false`; slower)

**Returns:** `{ path, recursive, count, total_referencers, redirectors: [{ name, package_path, package_name, asset_path, referencer_count, destination }] }`

### fixup_redirectors

Scriptable "Fix Up Redirectors in Folder": resave the packages still pointing at
the old paths, then (by default) delete the redirectors. Returns a `job_id`.

**Parameters:**
- `path` (string) - Root folder (default: `/Game`)
- `recursive` (bool) - Recurse into subfolders (default: `true`)
- `delete_redirectors` (bool) - Delete the redirectors after fixing (default: `true`)
- `batch_size` (int) - Redirectors fixed per editor tick (default: `1`)

**Returns:** `{ job_id, state, count, delete_redirectors }`

### move_assets

Move/rename assets in batch via `IAssetTools::RenameAssets`. Returns a `job_id`.

**Parameters:**
- `moves` (array) - `[{"source": "/Game/Old/Foo", "destination": "/Game/New/Foo"}, ...]`
- `assets` (array) - `["/Game/Old/Foo", ...]`, used with `destination_path`
- `destination_path` (string) - Destination folder for `assets` (each keeps its name)

Provide either `moves`, or `assets` with `destination_path`. UE leaves a
redirector at each old path; clear them with `fixup_redirectors`.

**Returns:** `{ job_id, state, count }`

### resave_packages

Load and save packages, resolving redirector imports and writing the new paths.

**Parameters:**
- `packages` (array) - Explicit package names, or
- `path` (string) + `recursive` (bool) - Resave every package under a folder

**Returns:** `{ job_id, state, count }`

### get_job_status

Poll a job started by the tools above.

**Parameters:**
- `job_id` (string) - The id returned when the job was started

**Returns:** `{ job_id, kind, state, phase, done, total, error, items, log }` where `state` is one of `queued | running | done | failed`.

## Typical redirector-cleanup flow

```json
{ "command": "list_redirectors", "params": { "path": "/Game/Art", "recursive": true } }
{ "command": "fixup_redirectors", "params": { "path": "/Game/Art", "recursive": true } }
{ "command": "get_job_status", "params": { "job_id": "<id from fixup_redirectors>" } }
```
