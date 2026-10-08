# Unreal MCP Asset Tools

Tools for organizing assets: moving/renaming them and cleaning up the
ObjectRedirectors UE leaves behind (the "sorting" operations that otherwise had
to be scripted through `execute_python`).

## Long-running jobs

`move_assets`, `move_folder`, `fixup_redirectors`, and `resave_packages` run as
**asynchronous jobs** on the Unreal side. Poll the returned `job_id` with
`get_job_status` until `state` is `done` or `failed`. Dry runs return a mapping
without creating a job. Native rename/fixup calls can still block the editor
thread while that individual operation runs.

Only one of these asset mutation jobs can run at a time. Wait for it to finish
before starting another. This guard does not prevent manual editor operations,
imports, or arbitrary Python scripts from modifying the same assets.

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

Stops with `state=failed` if cleanup cannot be verified, rather than reporting
unconditional success. Redirectors are loaded per tick, not retained as raw
object pointers across ticks.

### move_assets

Move/rename assets via `IAssetTools::RenameAssets`. By default, each asset is
renamed, saved at its destination, and its source redirector is fixed through
Unreal's `FixupReferencers`. The job checks that the registry/disk no longer
contain the source package and that the registry reports no remaining source
referencers before advancing to the next asset.

**Parameters:**
- `moves` (array) - `[{"source": "/Game/Old/Foo", "destination": "/Game/New/Foo"}, ...]`
- `assets` (array) - `["/Game/Old/Foo", ...]`, used with `destination_path`
- `destination_path` (string) - Destination folder for `assets` (each keeps its name)
- `dry_run` (bool) - Validate and preview without changing assets (default: `false`)
- `fixup_redirectors` (bool) - Fix and verify source redirectors after every move (default: `true`)

Provide exactly one input form. Package paths and matching object paths such as
`/Game/Old/Foo.Foo` are accepted; subobject paths are rejected. All requests are
validated before mutation: invalid/missing sources, redirector sources, duplicate
sources or destinations, self moves, and occupied destinations are rejected.
No overwrite, swaps, or moves into another source package are supported.

**Returns:** `{ dry_run, fixup_redirectors, count, moves: [{source, destination}] }`,
plus `{ job_id, state }` when executing. `done` counts fully completed moves.
A failure after rename may leave the current asset at its destination even when
it is not included in `done`; see recovery below.

### move_folder

Move the **contents** of a content folder, preserving relative subfolders.
For example, `/Game/Old/Materials/M_Wall` becomes
`/Game/Art/Materials/M_Wall` when moving `/Game/Old` to `/Game/Art`.

**Parameters:**
- `source_path` (string) - Source content folder
- `destination_path` (string) - Destination content folder
- `recursive` (bool) - Include subfolders (default: `true`)
- `dry_run` (bool) - Validate and return every planned move (default: `false`)

Uses the same preflight and per-asset cleanup as `move_assets`, with cleanup
always enabled. Existing redirectors are excluded from the move list, not
silently deleted. Equal or nested source/destination folders are rejected.
Empty folders or folders containing only redirectors return an error.
Source folders are not deleted. Use content paths, never filesystem moves of
`.uasset` or `.umap` files.

**Returns:** The same mapping/job response as `move_assets`; job `kind` is
`move_assets`.

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

## Recommended restructuring flow

Preview first:

```json
{ "command": "move_folder", "params": { "source_path": "/Game/Old", "destination_path": "/Game/Art", "dry_run": true } }
```

Inspect the mapping, then execute and poll until terminal state:

```json
{ "command": "move_folder", "params": { "source_path": "/Game/Old", "destination_path": "/Game/Art" } }
{ "command": "get_job_status", "params": { "job_id": "<returned id>" } }
```

For selected assets or renames, use `move_assets` with the same preview pattern.
No separate cleanup call is required for default moves.

### Partial failure recovery

On `failed`, stop and inspect `error`, `phase`, `items`, and `done/total`.
Earlier moves are not rolled back. A failed rename/save/fixup can already have
changed the current asset, so do not blindly retry the original batch.

1. Inspect both paths in the reported failed move.
2. Resolve read-only/source-control issues and save any unsaved destination.
3. List and fix remaining redirectors under the **old** source folder, then poll
   that cleanup job. Do not force-delete unresolved redirectors.
4. Build a new preview containing only assets still at their original paths.

Verification is based on Unreal's package/asset registry, not an exhaustive scan
of string paths in external files or custom serialized data.

## Standalone redirector-cleanup flow

```json
{ "command": "list_redirectors", "params": { "path": "/Game/Art", "recursive": true } }
{ "command": "fixup_redirectors", "params": { "path": "/Game/Art", "recursive": true } }
{ "command": "get_job_status", "params": { "job_id": "<id from fixup_redirectors>" } }
```

## Dependency / reference graph

`get_asset_graph` answers "what does this load" and "what loads this" from the
AssetRegistry, so an impact check can run before a move. Pass a package path or
an object path; a stale (redirector) path is followed to the real asset.

```json
{ "command": "get_asset_graph", "params": { "asset_path": "/Game/Art/MI_Wood" } }
```

Returns `dependencies` (packages this asset imports) and `referencers`
(packages importing this asset). Restrict with `direction` =
`dependencies` | `referencers` | `both` (default).

## Deleting assets

```json
{ "command": "delete_assets", "params": { "asset_paths": ["/Game/Art/Old_01", "/Game/Art/Old_MI"] } }
```

Each path is deleted, then the redirector the delete leaves behind is verified
gone with the same cleanup machinery as a move. An asset that is still
referenced is refused unless `force:true`, and the referencing packages are named
in the reply, because deleting a referenced asset silently nulls the reference
in its consumers. Returns `deleted`, `failed`, `deleted_paths`, and `failures`
(each naming the path and the reason); a `success:false` reply carries the
failure summary in `error`.
