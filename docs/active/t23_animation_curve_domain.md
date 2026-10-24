# T23 Animation/Curve HJSON Domain

Animation/Curve **HJSON** authoring uses
`client.content_graph.import_inline(target, document)` for inline documents and
`workspace.import_changes()` for an exported workspace. Independent readback uses
`client.content_graph.structure(...)` or a fresh export. The shared
`client.content_graph.export_workspace(...)` command publishes the registered
AnimSequence, BlendSpace or AnimMontage authoring workspace for existing assets. That
semantic workspace exposes declared root UObject properties and AnimSequence
`animation` values through shared property semantics and content-file organization. Bone tracks
are keyed by native bone name; frame rate uses the shared reflected struct codec.
Float curves and the nine position/rotation/scale transform subcurves use the
shared rich-curve format, including key interpolation and tangents. Scalar curves
also expose extrapolation. UE5.6's Sequencer controller does not implement transform
child extrapolation edits, so those fields and unsupported curve defaults are omitted.
Supplying these unsupported fields is rejected at their exact input location,
including a disabled `default_value`; they are not silently discarded after apply.
The generated `examples/files/.../float_curves/index.hjson` and corresponding
`transform_curves` guide provide complete new named-curve shapes. Every scalar
curve contains `channels.value`; transform curves nest these under
`position/rotation/scale` and `x/y/z`. These examples reuse the animation owner's
writable projection. CurveFloat/Vector/LinearColor assets separately publish
their complete templates through the shared curve value definition, retaining
their native default-value support. Runtime curves and CurveTable row schemas
continue to use their existing native templates.
`notify_tracks`, `notifies` and `sync_markers` use shared ordered record bindings and
property application; notify seconds/duration use native linkage and cache refresh.
Notify object creation uses `asset.owned_candidates` for `Notifies[index].Notify`
or `Notifies[index].NotifyStateClass`; put the returned name in the ordinary
`owned_object` value. Persona supplies the class picker and native creation policy,
while shared authoring handles selection and recursive properties. `animation.metadata`
is the package's object metadata string map; it is separate from instanced UAnimMetaData.
Candidate lookup resolves new array positions in a scratch owner and uses the
registered native editor's picker; it does not require a pre-existing Notify.
Notify `settings` is the native Details category/property map (`AnimNotifyEvent`
contains typed `NotifyName`, `Notify` and `NotifyStateClass`), whereas track/marker
`settings` is a struct value with `fields`. The public SDK T23 document at
`UeAgentInterfaceCMD/docs/active/t23_animation_curve_domain.md` contains the first-row
workspace example. New records omit `instance`; retained records keep it.
The external-import provenance namespace remains owned by its import provider.
Ordinary `content_ref` values locate workspace-relative HJSON files containing
`value`: named bone samples and named curves are separate editing units, while
each curve retains its keys/tangents and each bone retains its full sample track.
Montage slots/sections and animation events use the same public content organizer;
aligned PoseAsset data stays together. Edit the referenced value, then use the
same `workspace.import_changes()`. The common resolver assembles the original
property before the existing animation provider; no animation-specific file parser
or manual manifest/hash synchronization is needed.
Curve decoding carries an escaped JSON Pointer from the failing field through
the animation provider and content references to the original file and line.
The shared curve codec uses the same path contract for curve assets, CurveTable
rows and embedded property curves; display names are never parsed as dot paths.
`command_search`, the generated SDK, formal
schema, and `ueagentinterfacecmd` expose the same current contracts. There is no task-local generated
authority.

T13 owns initialization, recursive traversal, default resolution, binding,
failure, rollback, and cleanup. The T23 provider consumes that common contract
and registers `/Script/Engine.AnimSequence` and
`/Script/Engine.BlendSpace` and `/Script/Engine.AnimMontage`; it has no private recursive or initialization
path.

Native controllers retain responsibility for curves, keys, compression and cache
refresh; the shared authoring transaction owns save/reload and independent readback.
T56 describes the retained native compatibility contracts.

## Missing-root animation documents

The registered Animation family uses `schema: "uai.animation_curve.edit/1"`.
Creation settings are directly under the literal `root.create`. Pass the target
separately to `client.content_graph.import_inline(target, document)`; the public
entry supplies the missing-target expectation and source carrier. The complete
Montage document is:

```hjson
schema: "uai.animation_curve.edit/1"
root: {
  type: "/Script/Engine.AnimMontage"
  create: { source_animation_ref: "/Game/Example/AS_Source.AS_Source" }
}
```

Use an actual source reference and a new target such as
`/Game/Example/AM_New.AM_New`. The same outer shape applies to the following
registered types; substitute only `root.type` and `root.create`:

| Asset | `root.type` | Required `root.create` fields |
| --- | --- | --- |
| AnimSequence | `/Script/Engine.AnimSequence` | `skeleton_ref` supplies the skeletal context; an empty sequence may omit it |
| Montage | `/Script/Engine.AnimMontage` | `source_animation_ref` or `skeleton_ref` |
| PoseAsset | `/Script/Engine.PoseAsset` | `source_animation_ref` |
| BlendSpace 1D | `/Script/Engine.BlendSpace1D` | `skeleton_ref`, `dimension: 1` |
| BlendSpace 2D | `/Script/Engine.BlendSpace` | `skeleton_ref`, `dimension: 2` |

BlendSpace creation may include `root.axes` and `root.samples` in the same import;
their shape is the current native definition contract. Omitted definitions retain
UE factory defaults. AnimSequence creation uses the native factory and the same
Animation transaction as edits. Export the result for ordinary workspace editing.
Existing non-directory AnimSequence change documents may combine
`metadata_changes`, `curve_changes`, and `event_changes`. They are prepared on
one proposed asset and applied in one shared transaction, including the retained
Timeline compatibility bindings. A later invalid change cannot leave an earlier
category committed. Verification compares the complete sequence document.
Within `event_changes`, `event_ref` first selects the exported `instance` in its
event collection; a unique name remains accepted for compatibility. Ambiguous
names return `animation_event_ref_ambiguous` instead of modifying the first row.
New events use a new name. Re-export after applying changes before selecting rows
for another import. New notification settings contain only the fields authored
by that input, so the native Notify/NotifyState class supplies its default trigger
weight threshold. Existing notification settings are preserved.

Missing-root BlendSpace creation is supported. Its `create` object
must identify a skeleton and dimension `1` or `2`; axis/sample definitions can
accompany creation. Existing native BlendSpace document edits require
the current persisted-package revision plus explicit `axes` and `samples`.
Ordinary workspace exposes the reflected `BlendParameters` and `SampleData`
properties through the shared struct/array codec. The Animation lifecycle validates
samples and rebuilds native sampling data in both the proposed and actual asset.
Independent
readback is structural even when the BlendSpace has no samples; it does not
misroute an empty asset through the sampling command.

Existing AnimSequence HJSON editing uses its current revision. Missing-root
creation uses `root.create.skeleton_ref` and may carry the same change collections.
Montage inline creation uses
`root.type: "/Script/Engine.AnimMontage"` and `root.create.source_animation_ref`
or `root.create.skeleton_ref`, through UAnimMontageFactory and the shared asset lifecycle.
Unregistered
creation, stale revisions, or native contract failures produce typed errors
without a false-success readback.

Property and AnimSequence structure imports use one shared semantic workspace
coordinator and Animation provider transaction for mutation, saving, package reload,
and final readback. Bone/curve mutations use the native DataModel Controller;
bracket completion is followed by `WaitOnExistingCompression` before scratch release
or persistence. Frame/key indices use the model frame rate; rich-curve time is seconds.
Late readback rejection restores the prior dirty in-memory state and original
package bytes independently. Skeleton reassignment remains outside the ordinary root
edit. AnimSequence exports its current `skeleton` as read context for valid bone names.
Unedited bone tracks
and curves are retained; a supplied named track/curve map is its desired collection.
Existing AnimSequence fixtures can be prepared with the public
`client.asset.object_clone(...)` command; this is a copy of an existing animation,
not missing-root AnimSequence creation.

Montage `animation.slots` contains ordered `segments`; `animation.sections` contains
section settings, time in seconds, `next_section` and scoped `properties`. Retained
records use their exported `instance`; new records omit it. Segment start positions
are computed by native `SortAnimSegments` from the authored order and lengths,
not exposed as writable reflected fields. Section linkage is a domain structure
operation, separate from editable Details properties. Section `MetaData` uses the
shared owned-object value; `asset.owned_candidates` accepts
`CompositeSections[0].MetaData`. The native EditorCompositeSection wrapper supplies
its Details context, while the final object belongs to the Montage.

Implementation and remaining verification status are maintained in the active plan's
Animation I06 entry; this contract does not declare Animation-family completion.
