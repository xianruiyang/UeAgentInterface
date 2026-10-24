# Rigging Structured Authoring

## Scope

The `rigging` authoring domain owns the unified structured workflow for:

- `control_rig` (`UControlRigBlueprint`)
- `ik_rig` (`UIKRigDefinition`)
- `ik_retargeter` (`UIKRetargeter`)

## Public workspace

Normal editing uses `client.content_graph.export_workspace(target, profile_id='llm_authoring')`,
the returned ordinary HJSON files, and `workspace.import_changes()`. Re-export separately to read back the result.
The shared workspace owns source recovery, value decoding, keyed desired state, transactions and persistence.
The one-file domain envelope described below is the retained domain/compatibility representation, not the normal model workflow.

Control Rig hierarchy uses the `rig_elements` map keyed by native `kind:name`.
Its graph collection uses `rig_graphs`, keyed by the exported native graph name; each graph keeps `nodes` and `links` together.
This preserves the distinction between the main RigVM model and the function library even when both are empty.
Use the exported node and pin aliases for defaults and connections. Vector components accept the shared compact array format.
New nodes in the default RigVM graph use `candidate: { name: "<queried name>" }` and a position.
Candidate names come from `client.animation_rig_graph.candidates(target, query=...)`; supported native menu actions
are invoked on the current Blueprint. Creation support and exported definitions share the unit, template, variable,
enum, function-reference and invoke-entry families. Candidate description reports the native `action_kind`.
Unresolved templates retain their notation until native pin/link operations resolve them.
Re-export after resolution: the native node title, and therefore its public alias, can change. Function-library creation is not covered by the default-graph candidate context.

IK Rig uses the same keyed desired-state projection for `ik_goals`, `ik_solvers` and `ik_chains`.
The exported `solver_order` is an editable scalar array, not a fixed collection of item references.
IK Retargeter uses `ik_retarget_ops`, `ik_retarget_mappings` and `ik_retarget_poses`; the latter keeps
the `source` and `target` side names, pose names and coordinate-space declarations together. `op_order` is a scalar array.
These collections retain the native domain fields described below and share the workspace's identity and import machinery.

## Domain representation

The retained single-file domain representation uses `index.hjson`. Directory Apply rejects unknown files. Export builds the complete one-file plan and publishes it through the owned staging publisher; it does not reset final before staging and writes only `index.hjson`.

Export and Apply are independent contracts. Export exposes only identity and editing-relevant state. Apply may accept operation controls and recursively structured input that Export does not emit.

## Root Document

```hjson
operation: update
target: { asset_path: "/Game/Rigs/Example", asset_kind: "control_rig" }
properties: []
provider_extension: {
  compile: true
  save: false
}
```

`operation` is one of `create`, `update`, `upsert`, `clone`, or `remove`. `clone` also requires `target.source_asset_path`.

Create begins with the native UE factory seed. Omitted fields retain UE factory defaults. Update and clone begin with a duplicate of the existing or source object, so omitted fields preserve source state. Apply never invents missing engine-owned defaults.

Root `properties` uses the shared recursive UObject/UStruct property descriptor contract. Properties already owned by `provider_extension`, such as preview meshes, shape libraries, and Retargeter rig references, are excluded from the root list so every field has one writer. The domain document uses UE controllers for hierarchy, RigVM, solver, chain, mapping, and pose mutation. Both sections participate in the same preflight, transaction, rollback, compile/save hooks, and state-snapshot verification.

## Control Rig

`provider_extension` accepts:

- `rig_type`: native Control Rig class/module state, preserved on export. An unchanged value is accepted; changing this state is not a document property assignment. UE's native rig conversion can rebuild the hierarchy and graphs and is outside this document edit contract.
- `preview_mesh`: skeletal mesh asset path.
- `shape_libraries`: ordered `array<ue_asset_path>`.
- `hierarchy`: element map keyed by `kind:name`.
- `graphs`: graph map; each graph contains a node map and `links: array<[source_pin, target_pin]>`.
- `variables`: descriptor object containing `schema`, `variable_count`, and an ordered `variables` array.

Hierarchy transforms are authoring initial transforms in `parent_local_initial` space. Transient current transforms are not exported. Control settings are recursively encoded as deltas from a UE-initialized native struct. Omitted fields retain native defaults, and disabled limits omit their irrelevant minimum and maximum values. Control initial values retain their native scalar/vector/rotator/transform shape.

`bone_type` supplies the native origin for a newly created bone. Existing bones retain their native origin; an unchanged exported value is accepted. A different value is reported at the authored field instead of being silently ignored. Bone transforms, parents, control settings and other editable contents retain their normal native edit paths.

Omitting `hierarchy.elements` preserves the collection. A supplied element map is desired state for the exported bone/null/control/curve elements: omitted elements are removed through the native controller, including imported bones. Other engine-owned element kinds retain their native owner. On retained elements, omitted parents, transforms, settings and values preserve native state; `parent: null` explicitly clears the parent. Native creation identities and requested parents are checked after the controller call, so a rejected native relation cannot report success.

New controls use UE-initialized `FRigControlSettings` when settings are omitted and the settings' native identity value when `initial_value` is omitted. Supplied settings remain recursive deltas and supplied initial values retain their native type. Native float control components, curve values and transforms use the shared codec's actual storage width; integer controls retain exact input integers.

RigVM node positions use `rigvm_graph_canvas_2d`; they are not world or hierarchy coordinates. Node `pin_defaults` contains one editable value per root input/IO pin, plus the variable node's `Value` pin, keyed by public pin alias. A struct/array root value includes its children; the full `pin_interface` still exposes child endpoints for links. Empty strings and `()` are explicit values, not omitted defaults. Variable definitions no longer repeat the value as `default_value`.

Older documents may contain both `default_value` and `pin_defaults.Value`, or parent and child defaults. On existing nodes, unchanged entries are filtered against the native state before any default writes. Changed parents apply before explicit changed children. Conflicting changed values for the same pin produce a located diagnostic; new nodes treat every supplied default as explicit. Candidate creation fills missing native definition fields without replacing supplied defaults or function variable mappings.

Link endpoint pairs avoid repeating each endpoint as both a map key and a value. The supplied graph is desired state: surviving nodes retain their native identity, while omitted nodes and links are removed through the native controller. Retaining an exported node does not recreate it.

Blueprint variable names are preserved exactly, including meaningful whitespace. Variable defaults are associated through the UE variable GUID and the shared canonical property descriptor; Apply does not trim names or invent a second property-path rule.

创建 hierarchy curve 时，`value` 不只用于建立 curve storage：Apply 在 `AddCurve` 后通过 Rig Hierarchy API 显式写入请求值，因此第一次 Export→Apply→readback 就必须得到该值。更新已有 curve 使用同一写入语义。

## IK Rig

`provider_extension` accepts:

- `preview_mesh`: skeletal mesh asset path.
- `retarget_root`: bone name.
- `goals`: `map<goal_name, goal>`.
- `solvers`: `map<solver_id, instanced_struct>` plus `solver_order: array<string>`.
- `chains`: `map<chain_name, retarget_chain>`.

Goal `current_transform` is explicitly `character_global`. Goal-specific editable UObject properties use the shared property codec.

Goal 的 bone 绑定只在请求值与当前值不同时调用 `SetGoalBone`。等价重放保持 no-op；无效 bone 不能借由重复写入破坏已经正确的绑定。

A chain emits a non-empty `goal` only when that goal exists in the IK Rig. Dangling engine-side goal names are normalized to an empty optional reference instead of being presented as an applicable edit.

Each solver carries `struct_path` and a recursive typed `value` containing only fields that differ from a UE-initialized native struct. Apply resolves the actual `UScriptStruct`, initializes it through UE, decodes the supplied partial value, and rebuilds the controller-owned stack in `solver_order`. This supports engine and plugin solver structs without per-solver conversion code.

Solver and Retargeter op values use the shared native struct codec. Non-Edit persisted containers such as
`Settings` remain visible; transient and deprecated fields follow the codec's normal exclusions.
Top-level native-default fields are omitted, while a changed container retains its recursive value.
This differs from UObject Details eligibility, which would otherwise hide the settings container entirely.

## IK Retargeter

`provider_extension` accepts:

- `rigs`: `source` and `target` IK Rig asset references.
- `preview_meshes`: `source` and `target` skeletal mesh asset references.
- `ops`: `map<op_id, instanced_struct>` plus `op_order: array<string>`.
- `mappings`: map keyed by `op:target_chain`.
- `poses`: explicit `source` and `target` pose collections.

Retarget ops use the same native-default `UScriptStruct` delta codec as IK solvers. Apply first validates the complete parent graph, materializes parents before children, and then restores the explicit execution `op_order`. An omitted or empty parent means no parent.
Op identity, parent, enabled state and chain mappings are exported only through their explicit document fields;
the corresponding native `Name`, `ParentOpName`, `bIsEnabled` and `ChainMapping` fields are excluded from `value`.

Pose root translation is `retarget_root_global_translation_delta`. Bone rotation offsets are `bone_local_rotation_delta`. Source and target are never inferred from an unlabeled transform.
The native retarget pose stores only vertical root translation: nonzero X/Y values are diagnosed rather than silently cleared.
Rotation edits require the selected side's IK Rig and a bone in its skeleton. A mapping source is checked against the rig's declared chains,
not merely chains already mapped; `None` still clears a mapping, and incomplete rig configuration retains UE's unresolved-mapping semantics.
The original decoded pose and mapping requests are checked again after native cleanup. UE's default pose cannot be removed;
its native removal rejection is reported. Pose identities retain native `FName` case semantics.

## Export Profiles

- `llm_authoring`: identity and authoring state; default.
- `readback`: adds readback fields and may retain `value_text`.
- `debug`: adds diagnostics and may retain `value_text`.
- `validation`: minimal identity and contract-required state for validation.

Known vector, rotator, quaternion, color, and transform values use numeric component arrays. Transform keeps named `translation`, `rotation`, and `scale` members whose values are arrays. Generic structs recurse through known basic types. Containers declare and preserve their element, key, and mapped types.

All floating-point encoding canonicalizes negative zero to `0`. This does not change UE numeric meaning and prevents `-0`/`0` churn across Apply round trips.

`operations`, `control_type`, `kind`, `property_path`, and `category_path` are not emitted unless required by the applicable shared contract. `value_text` is omitted from `llm_authoring` unless no reliable structured `value` exists.

## Verification

Apply preflights the complete workspace without mutating the target. Successful execution compares expected and actual shared state snapshots. Create, update, clone, remove, compile, save, and verification failures share one rollback path. Reapplying an equivalent update with `compile: false` and `save: false` is reported as a no-op.
