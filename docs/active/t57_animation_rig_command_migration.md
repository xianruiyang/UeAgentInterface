# T57 Animation rig command authority

Current default methods:

```python
client.animation.control_rig_asset_create(...)
client.animation.control_rig_hierarchy_graph_apply(...)
client.animation.control_rig_shape_library_apply(...)
client.animation.control_rig_shape_library_asset_create(...)
client.animation.ik_retargeter_asset_create(...)
client.animation.ik_retargeter_definition_apply(...)
client.animation.ik_retargeter_settings_apply(...)
client.animation.ik_rig_asset_create(...)
client.animation.ik_rig_definition_apply(...)
client.animation.ik_rig_definition_auto_generate(...)
client.animation.skeleton_compatibility_apply(...)
client.animation.skeleton_definition_apply(...)
client.animation.skeleton_slots_read(...)
client.animation.skeleton_virtual_bone_apply(...)
```

They are discovered through command_search and command_detail and mirrored by
uai-cli exact execution. The animation-rig command owner delegates native
creation, hierarchy/graph/definition/settings mutation, generation,
persistence, readback, and cleanup to the common services. The SDK contains no
parallel rig or spatial business.

semantic_frame_id, write/read/verify frames, unit, basis_evidence, and
conversion_chain remain explicit. Terminal writes require independent
persisted readback. Prewrite failures leave no residue; unknown state is
reconciled. Cleanup is bounded and independently verified.

