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
