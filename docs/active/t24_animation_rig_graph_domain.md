# T24 Animation Rig Graph HJSON Domain

`animation-rig-graph-domain-owner` is the only public owner of
`animation_rig_graph.candidates` and the T24 HJSON domain projection. The
public candidate method is `client.animation_rig_graph.candidates(...)`.
For edits, obtain `workspace = client.content_graph.export_workspace(...)`,
edit its ordinary HJSON files and call `workspace.import_changes()`. For unknown command names or parameter shapes, follow [public command discovery](../PYTHON_SDK_AND_CLI.md#command-discovery).

## Candidate search boundary

`animation_rig_graph.candidates` currently accepts a ControlRigBlueprint and
enumerates the native `FRigVMActionMenuBuilder` menu for its default RigVM
graph in blank context. It initializes the public model-to-editor-graph delegate
used by the editor, but does not open an asset window. The search result returns
all matching readable names, including actions without a creation binding.
Explicit `describe={name, select?}` reports an action's creation support;
actual creation rejects unsupported actions with their specific reason.
Only an actual source-enumeration gap makes the catalog incomplete.
Unit, template, variable, enum, function-reference and invoke-entry families have
stable definitions; creation support is checked on the selected native action.

This is not a claim about pin-drag, selected-node, selected-asset, or every other
Control Rig editor context. IKRigDefinition, IKRetargeter, and Skeleton remain
valid T24 HJSON/readback roots, but they are not targets of this RigVM candidate
command.

Candidates are returned as all matching base names, grouped for display only.
Same-name identities are resolved through short semantic `select` fields, including
form, unit/method or function/library when available from the native spawner.
Support semantics are available from the candidate description when needed;
normal search does not expose machine identity, revision, paging, or full rows.
The selected candidate's description also reports its native `action_kind`,
so a display name is not used to guess whether it creates a unit or function.

## Missing-root asset creation

Submit this complete domain document with
`client.content_graph.import_inline(target, document)`; `target` is the
independent destination object path:

```hjson
schema: "uai.animation_rig_graph.edit/1"
root: {
  type: "/Script/ControlRigDeveloper.ControlRigBlueprint"
  create: {
    skeleton_ref: "/Game/Example/SK_Skeleton.SK_Skeleton"
  }
}
```

The referenced source must already exist. Choose the root type and required
creation references together:

| Root type | Required fields in `root.create` |
| --- | --- |
| `/Script/ControlRigDeveloper.ControlRigBlueprint` | `skeleton_ref` |
| `/Script/IKRig.IKRigDefinition` | `skeletal_mesh_ref` |
| `/Script/IKRig.IKRetargeter` | `source_ik_rig_ref`, `target_ik_rig_ref` |

The destination supplies the asset name and package path. Skeleton and
OptimusDeformer are readable domain roots but are not supported by this
missing-root creation route. Export a newly created asset before editing its
native document; do not include a root `operation` in the creation document.

## Minimal HJSON creation

A RigVM create edit uses the complete returned `candidate.name`, including its
readable scope when present. Control Rig does not use a separate `select` map.
The service re-enumerates the current context. Instance identity and graph placement remain outside the
candidate:

```hjson
edits: [{
  intent: create
  semantic_id: "rigvm_node:AddSpaces"
  value: {
    candidate: { name: "Add Spaces" }
    position: { x: 240, y: 144 }
  }
}]
```

When the model already knows more, `script_struct_ref` and `method` may be
provided beside `candidate` as assertions. They do not replace the readable name.
Hierarchy parent, transform/space contracts, graph context, instance identity,
placement, pins, links, defaults, and properties never become candidate
identity.

Preflight re-enumerates the current native blank-context menu, resolves the
selector, and rejects search-only action kinds before the first write. Commit
re-enumerates again and invokes the selected action's own `PerformAction`.
The owner may rename the resulting model node to the `semantic_id` suffix,
then records the actual node family's stable definition and position for independent
readback. Candidate creation does not fall back to `AddUnitNodeFromStructPath` or a copied
class/struct catalog.

Update and delete remain existing-node operations and do not pretend to be
candidate creation.

Ordinary workspace export retains the native family: unit struct/method,
template notation/resolved types, variable name/type object/getter direction,
enum type, function host/name/variable mapping, or invoked entry. Local function
hosts use `self`; external hosts retain their reference. Creating from an
exported definition uses the matching native controller operation. Definitions
of existing nodes must still identify that node; defaults, placement and
function variable mapping are applied separately.

## Domain lifecycle and other roots

The `uai.animation_rig_graph.edit/1` provider consumes the shared resolved
workspace lifecycle. T13 alone is the recursive initialization authority and
owns inline, single-file, and directory loading; T24 selects the declared
`entry_file`, routes the registered
root and operation, and rejects legacy top-level `carrier` and
`hjson_document` fields.

Edit existing Control Rig hierarchy and shape libraries, IK Rig definitions,
IK Retargeter settings, and Skeleton definitions through the exported HJSON
directory and `import_changes`. Inline and single-file imports initialize a
missing target. To auto-generate an existing IK Rig, call the separate
`animation.ik_rig.definition.auto_generate` operation. Each domain keeps its
own parent, transform, chain, and space contracts.

An IK chain uses its map key as its name. Omitted endpoints and goal retain an
existing chain's values, or the native unset defaults for a new chain. A new
chain need not select a goal; actual endpoint changes still use the native bone
setters. Explicitly supplied fields are type checked at their HJSON source.

Skeleton virtual-bone names follow UE's native `VB ` prefix (including the
space), for example `VB Torso`. Omit a new virtual bone's name to let UE generate
it. Failed native creation reports the name or source/target conflict at the
corresponding HJSON field.

Successful import compiles or validates through the native owner, saves,
unloads/reloads, and returns independent content-graph readback. Its public
success result is a short business projection containing the terminal status and
`target_ref`; revision, persistence receipt, and readback evidence remain
server-owned lifecycle facts. Missing roots,
stale revisions, target mismatch, ambiguous or unsupported candidates, invalid
assertions/spaces, unsupported root creation, or unregistered operations fail
before write or roll back without residue.

Existing Rigging assets consume root properties and document/layout edits through
the same semantic change projection, including callers without an explicit edited
unit list. Ordinary Control Rig node layout changes preserve non-target nodes.
An Update containing no document fields preserves the native document while
applying its root properties; explicit document input and Create dependencies
retain their domain validation. Callers do not need to resend an IK document for
a root-property edit.

Native Blueprint compilation, IK Rig reinitialization, IK Retargeter cleanup,
save, unload/reload, and final content-graph readback stay inside the shared
transaction. A late readback rejection restores the pre-import dirty in-memory
state and the original package bytes independently. Proposed native objects clear
public/standalone flags after duplication and remain transient until released.
The asset-delete lifecycle temporarily detaches the target IK Rig controller and
rebinds it by object path when the target survives a refusal or rollback; ordinary
external reference checks remain active. These paths do not establish complete
RigVM structure coverage.
