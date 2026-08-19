# T24 Animation Rig Graph HJSON Domain

`animation-rig-graph-domain-owner` is the only public owner for
`animation_rig_graph.candidates` and the T24 HJSON domain projection. The
public SDK calls are `client.animation_rig_graph.candidates(...)` and
`client.content_graph.import_workspace(...)`. Use `client.command_search(...)`
and `client.command_detail(...)` to inspect their live contracts; `uai-cli`
exposes the same discovery path. No task-specific helper or tutorial-owned
registry is authoritative.

`animation_rig_graph.candidates` is read-only and target-sensitive for exact
persisted ControlRigBlueprint, IKRigDefinition, IKRetargeter, and Skeleton
targets. It returns concrete native RigUnit struct identities with opaque
binding revisions, explicit placement-space requirements, and truthful bounded
paging.

The `uai.animation_rig_graph.edit/1` provider is a thin adapter over the shared
HJSON import lifecycle. It owns only classification, initialization-contract
selection, schema-to-command routing, and domain completion checks. T13 is the
sole common recursive initialization authority; T24 registers only the
Rig/IK/Skeleton root, control, bone, constraint, and space contracts and
consumes that seam. T13 alone loads inline, single-file, and directory carriers
and owns directory recursion; T24 consumes only the normalized
`source.kind=resolved` document set, selects exactly the declared `entry_file`,
and rejects legacy top-level `carrier` and `hjson_document` fields.

Writes delegate unchanged to the AnimationRig command owner for Control Rig
hierarchy and shape-library application, IK Rig definition and auto-generation,
IK Retargeter definition and settings, and Skeleton definition, compatibility,
and virtual-bone application. Every placement-bearing document carries the
exact `space_ref`, parent, and transform context required by the selected
native contract. The domain does not add a second asset factory, recursive
initializer, RigVM writer, compiler, persistence path, readback authority, or
cleanup path.

Successful import compiles or validates through the native owner, saves,
reloads, and returns independent `content_graph.*` readback. Missing roots,
stale revisions, target mismatch, invalid candidates or spaces, unsupported
root creation, or unregistered operations fail before write or roll back
without residue.
