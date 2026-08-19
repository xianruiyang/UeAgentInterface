# T58 Rig/spatial command authority

Current default methods:

```python
client.animation.ik_probe_candidate_apply(...)
client.animation.subject_morph_weights_update(...)
client.animation.subject_pose_update(...)
client.rig.control_bake_animation(...)
client.rig.control_bake_controls(...)
client.rig.ik_definition_update(...)
client.rig.retarget_assets_generate(...)
client.rig.retarget_definition_update(...)
client.rig.retarget_pose_auto_align(...)
client.rig.retarget_pose_update(...)
client.rig.skeleton_preview_mesh_bind(...)
client.rig.skeleton_socket_update(...)
```

They are discovered through command_search and command_detail and mirrored by
uai-cli exact execution. The rig-spatial owner uses a production binding seam
that resolves and freezes explicit request targets/inputs/context into real
native bindings, then delegates to the single TimelineAnimation and
RigSimulationSpatial ports. It must not use empty bindings, fixture-only
injection, Python binding state, or handler-side UE mutation.

semantic_frame_id, write/read/verify frames, unit, basis_evidence, and
conversion_chain stay explicit. Terminal operations require independent
readback in the declared state/frame. Prewrite binding/frame failure leaves no
residue; unknown state is reconciled. Cleanup is bounded and independently
verified.

