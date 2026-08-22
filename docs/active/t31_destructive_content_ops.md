# T31 destructive content operation authority

The default SDK methods are `client.asset.delete`,
`client.asset.fixup_redirectors`, `client.asset.move`,
`client.asset.object_clone`, `client.modeling.actor_mesh_replace`,
`client.modeling.asset_material_slot_remove`,
`client.modeling.voxel_bounds_remove`,
`client.skeletal_mesh.remove_morph_target`,
`client.skeletal_mesh.remove_skin_weight_profile`,
`client.world.data_layer_delete`, `client.world.level_content_delete_scope`,
`client.world.level_streaming_move_actors`,
`client.world.level_streaming_remove`, and `client.world.pcg_cleanup`.
Callers discover current fields and hashes with `command_search` and
`command_detail`; `uai-cli` exposes the same exact contracts. No task-local
binding is active.

The destructive-content owner performs complete preflight and, when required,
issues a bounded authorization identity before mutation. Execution rejects a
missing, stale, mismatched, or replayed authorization before write. A success
is terminal only after native mechanics, persistence, independent readback,
and cleanup are complete. The SDK and CLI contain no selection, authorization,
Unreal mutation, rollback, retry, or cleanup business logic.

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
