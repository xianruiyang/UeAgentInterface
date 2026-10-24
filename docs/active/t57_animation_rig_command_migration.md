# T57 Animation rig command authority

Current default methods:

```python
client.animation.control_rig_asset_create(...)
client.animation.control_rig_shape_library_asset_create(...)
client.animation.ik_retargeter_asset_create(...)
client.animation.ik_rig_asset_create(...)
client.animation.ik_rig_definition_auto_generate(...)
workspace = client.content_graph.export_workspace(...)
workspace.import_changes()
```

For unknown command names or parameter shapes, follow [public command discovery](../../../../UeAgentInterfaceCMD/docs/PYTHON_SDK_AND_CLI.md#command-discovery). Control Rig, IK Rig, and IK Retargeter creation remain
explicit until the generic asset candidate/create chain proves non-interactive
equivalence. Hierarchy, definition, settings, compatibility, and virtual-bone
mutation are authored through the AnimationRig HJSON root and
content_graph.import_workspace; their former typed command IDs remain internal
operation kinds only. Shape-library editing and Skeleton slots use ordinary
workspace export/import; their former typed apply/read identities are dormant.
Shape-library asset creation and IK auto-generate remain explicit active
capabilities. The SDK contains no parallel
rig or spatial business.

semantic_frame_id, write/read/verify frames, unit, basis_evidence, and
conversion_chain remain explicit. Terminal writes require independent
persisted readback. Prewrite failures leave no residue; unknown state is
reconciled. Cleanup is bounded and independently verified.

Control Rig hierarchy local, control offset, and shape transforms use native
double values in memory. UE's package serializer stores their compact form, so
saved readback can round components to float precision or elide near-identity
components. Verification uses that native serialization rule; other double
values retain their declared comparison semantics.
