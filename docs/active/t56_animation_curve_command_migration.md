# T56 Animation curve command authority

Current default methods:

```python
client.animation.blend_space_asset_create(...)
client.animation.blend_space_definition_apply(...)
client.animation.blend_space_sample(...)
client.animation.montage_create(...)
client.animation.montage_read(...)
client.animation.montage_structure_replace(...)
client.animation.sequence_bone_tracks_replace(...)
client.animation.sequence_curves_update(...)
client.animation.sequence_events_update(...)
client.animation.sequence_metadata_update(...)
client.animation.sequence_pose_sample(...)
client.animation.sequence_read(...)
client.animation.sequence_root_motion_analyze(...)
```

They are discovered through command_search and command_detail and mirrored by
uai-cli exact execution. The animation-curve command owner delegates durable
mutations and read/sample analysis to the common services and native mechanics.
The SDK contains no animation, sampling, timebase, or spatial conversion
business.

Timebase/frame and semantic_frame_id/unit/basis_evidence/conversion_chain remain
explicit. Terminal writes require independent persisted readback. Prewrite
failures leave no residue; unknown state is reconciled. Cleanup is bounded to
owned fixtures and independently verified.

