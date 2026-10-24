# T56 Animation curve command authority

Current default methods:

```python
client.animation.blend_space_asset_create(...)
client.animation.blend_space_sample(...)
client.animation.sequence_pose_sample(...)
client.animation.sequence_root_motion_analyze(...)
workspace = client.content_graph.export_workspace(...)
workspace.import_changes()
client.content_graph.import_inline(...)
```

For unknown command names or parameter shapes, follow [public command discovery](../../../../UeAgentInterfaceCMD/docs/PYTHON_SDK_AND_CLI.md#command-discovery). The animation-curve command owner delegates durable
mutations and read/sample analysis to the common services and native mechanics.
The SDK contains no animation, sampling, timebase, or spatial conversion
business.

The common descriptor-to-command projection preserves each parameter's full
schema through the manifest and generated SDK. Read it on demand with
`ueagentinterfacecmd describe animation.sequence.pose.sample --parameter input`
or `uai.describe_command(...)`; `--pointer /properties/time_point` selects the
point shape. The same metadata path covers root-motion ranges and other domains.
Input diagnostics include the failed keyword's `expected` constraint without
changing validation rules or error codes.

BlendSpace definitions, AnimSequence bone tracks/curves/events/metadata, and
Montage structure use ordinary HJSON workspace export/import. Missing Montage
targets can be created with `import_inline`. Their former typed mutations and
asset `read` commands are dormant compatibility identities, not default named
SDK methods. Pose, root-motion and BlendSpace sampling remain separate active
queries; workspace export does not replace evaluation.

AnimSequence pose/root-motion query points use the exact native clock
`animation_sequence:<full UObject path>` and the asset's native frame rate.
Root-motion analysis requires explicit lower and upper points; open bounds are
not an implicit full-animation range. Obtain duration/frame rate from the current
asset document and specify the intended interval. Semantic time errors identify
the required clock/rate/bounds rather than reporting a missing readback.

Timebase/frame and semantic_frame_id/unit/basis_evidence/conversion_chain remain
explicit. Terminal writes require independent persisted readback. Prewrite
failures leave no residue; unknown state is reconciled. Cleanup is bounded to
owned fixtures and independently verified.
