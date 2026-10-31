# T58 Rig/spatial command authority

Current default methods:

```python
client.animation.ik_probe_candidate_apply(...)
client.animation.subject_morph_weights_update(...)
client.animation.subject_pose_update(...)
client.rig.control_bake_context(...)
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

For unknown command names or parameter shapes, follow [public command discovery](../../../../UeAgentInterfaceCMD/docs/PYTHON_SDK_AND_CLI.md#command-discovery). The rig-spatial owner uses a production binding seam
that resolves and freezes explicit request targets/inputs/context into real
native bindings, then delegates to the single TimelineAnimation and
RigSimulationSpatial ports. It must not use empty bindings, fixture-only
injection, Python binding state, or handler-side UE mutation.

`rig.control.bake.context` is a read-only, request-bound query. It accepts the
LevelSequence asset as `target` and `input.sequence_ref`, an exact readable
`binding_name`, and the generated Control Rig class as `rig_ref`. It resolves
one native MovieScene binding and its skeletal component through the same
binding seam as baking. The result includes the native playback range, display
rate and current revisions as complete inputs for the existing bake commands;
optional `target_animation_ref` must identify an existing AnimSequence and adds
the animation export input. Missing or ambiguous bindings fail before mutation.
The query does not evaluate, create tracks, bake or save assets.

The single-record public projection is `bake_context`. The SDK exposes
`result.rig_bake_context` and retains binding/revision custody inside that
handle; callers use `bake_controls()` or `bake_animation()` and refresh the
query after editing or restarting Editor. Existing bake command signatures
and native mutation/settle/readback behavior remain unchanged. SDK usage is
maintained in CMD `docs/active/t58_rig_spatial_command_migration.md`.

semantic_frame_id, write/read/verify frames, unit, basis_evidence, and
conversion_chain stay explicit. Terminal operations require independent
readback in the declared state/frame. Prewrite binding/frame failure leaves no
residue; unknown state is reconciled. Cleanup is bounded and independently
verified.

