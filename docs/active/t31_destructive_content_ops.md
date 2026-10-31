# T31 destructive content operation authority

The default SDK methods are `client.asset.delete`,
`client.asset.fixup_redirectors`, `client.asset.move`,
`client.asset.object_clone`,
`client.modeling.voxel_bounds_remove`,
`client.skeletal_mesh.remove_morph_target`,
`client.skeletal_mesh.remove_skin_weight_profile`,
`client.world.level_content_delete_scope`, and `client.world.pcg_cleanup`.
For unknown command names or parameter shapes, follow [public command discovery](../../../../UeAgentInterfaceCMD/docs/PYTHON_SDK_AND_CLI.md#command-discovery).
The destructive-operation semantics below remain owned by this document.

## Destructive plan, authorize, and commit

The destructive-content owner performs complete preflight before mutation. A success
is terminal only after native mechanics, persistence, independent readback,
and cleanup are complete. The SDK and CLI contain no selection,
Unreal mutation, rollback, retry, or cleanup business logic.

Active clone and asset move execute directly when `confirm` is omitted;
`confirm=False` requests an optional preview and `confirm=True` does not require
a preceding matching preview. The dormant cross-level Actor move handler retains
the same confirmation behavior, but is not a normal authoring entry.

Move/rename changes the asset's path, not the bindings of an already exported
workspace. Preserve any unapplied edits, export the destination to a new
workspace, then apply those edits against its current interfaces. Clone also
keeps the source workspace bound to the source; export the target to edit the copy.

For destructive removal, call the generated method once with `confirm=False` to preview. Review the readable
impact and, after confirming the exact scope, repeat the same parameters with
`confirm=True`. Only the confirmed terminal success together with an independent
domain readback is success. An uncertain result must not be replayed automatically;
recover through the durable journal workflow.

## `asset.object.clone`

`source_ref` accepts an existing `/Game` or `/Engine` asset; `target_ref` names a
new `/Game` asset. Both accept a package path (`/Game/Folder/Asset`) or its matching
top-level object path (`/Game/Folder/Asset.Asset`). Internal identity and readback
use the resolved object path. A mismatched object suffix is an error, not a rename.
For a readable asset name from `query.content.search`, call `asset.registry_record`
with that complete name as `target`; its `assets[].object_path` supplies the source path.
Input failures retain their error code and expose `error.diagnostic.json_pointer`
(`/source_ref` or `/target_ref`) plus a readable correction. An invalid target
does not imply that the source mount is unsupported.

## `asset.delete`

Use `client.asset.delete(targets, recursive=False)`. `targets` is one non-empty
array of `/Game` asset object paths, package names, Content Browser package
folders, or a mixture; a single target is still a one-item array. Exact assets
resolve before folder expansion. `recursive=False` selects only assets directly
inside a selected folder, while `recursive=True` includes nested descendants.
Planning normalizes and sorts the actual assets, removes asset/folder overlap,
and returns the resolved `targets` with the bound impact and revision snapshot.

Commit reports every actual asset as `deleted`, `blocked`, or `remaining` with
its own reason and independent file/package/registry/object readback. Partial
deletion is a failed command with the complete projection, never an aggregate
success. UAI uses Unreal asset and package mechanics; it does not recursively
delete `.uasset`, `.umap`, or operating-system directories.

`rolled_back` confirms restored persisted content and required native asset state;
restoring files alone is insufficient. The original operation error is retained
separately from any unconfirmed recovery reported in `failure_reason`.

A clean open asset editor is closed by Unreal's normal deletion mechanism. An
empty package or a retired UObject may remain in memory until native references
are released: `package_empty` and `object_retired` report those states separately
from `package_absent` and `object_absent`. Successful deletion still requires the
file and registry entry to be absent, the package to be absent or empty, and the
object to be absent or no longer an asset with public/standalone ownership.

For an ordinary registered SoundSubmix, confirmed deletion also unregisters the
exact target from its audio devices and releases the native registration's root
after synchronization. Required/default device submixes and submixes with active
source voices are refused; native asset-reference checks remain in force. On
failure, durable recovery restores the asset and relationships, then rebinds its
captured registration and root state by asset path. This does not snapshot or
reconstruct transient recording buffers, DSP history, or external buffer-listener
state. A recovery that cannot verify the asset/registration terminal state remains
`state_unknown`; file deletion alone is not success.

## HJSON-owned destructive desired state

`world.data_layer.delete`, `world.level_streaming.move_actors`,
`world.level_streaming.remove`, `modeling.actor.mesh.replace`, and
`modeling.asset.material_slot.remove` are dormant compatibility identities. Their
persisted structure/removal intent belongs to the common HJSON desired-state diff
and import transaction. This assigns ownership; it is not a claim that every
successor is implemented. On applicable partitioned-World import paths, Level
HJSON provides Data Layer creation, initial state and membership addition. This
is not unconditional coverage for ordinary updates to any existing Level, nor
a complete existing-layer unload/restore workflow. Streaming relations, cross-Level Actor migration and
their initial-state round trip remain an incomplete HJSON capability.
The dormant identities do not appear in ordinary search or named SDK methods;
their exact native mechanics remain available only through the central dormant
compatibility path and ordinary exact dispatch. Destructive
removal still follows the content-removal confirmation described above.

`world.level_content.delete_scope` keeps `folder_path` as the required scope
boundary. Without `managed_id`, it is the explicit broad operation and includes
actors in that folder and its descendants. With `managed_id`, planning and
execution use the same exact managed-actor resolver: the world must contain
exactly one actor carrying both `UAI.Managed` and the canonical
`UAI.Id=<managed_id>` tag, and that actor must be in the exact `folder_path`.
Missing, duplicate, ambiguous, marker-less, or folder-mismatched identities
fail before mutation. The destructive plan projects that actor path rather than
the whole folder, so a same-folder sibling with a different managed identity is
outside the authorization and remains untouched.
