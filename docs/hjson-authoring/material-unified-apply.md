# Material Unified Apply

## Scope

Material authoring uses two operation contracts under the shared Apply workspace coordinator:

- `uai.apply_input.material.asset`: create, update, upsert, clone, remove, root properties, function settings, and full graph replacement for `UMaterial` and `UMaterialFunction`.
- `uai.apply_input.material.graph_mutation`: incremental expression create/update/remove, expression-to-expression connection, and expression-to-material-root connection.

Both contracts run full preflight before persistent mutation, capture rollback state, apply lifecycle hooks, optionally compile/save, and compare expected/actual state snapshots before commit.

The asset contract also accepts existing `UMaterialInstanceConstant` updates. Parameter
value properties use shared reflected capture/definitions/ordinary projection and
shared compact-value decoding into isolated native parameter carriers. Its
parameter intent is projected from the ordinary semantic workspace into the
ten native parameter kinds (including clearing overrides and explicit null textures), together with root properties in the
same ObjectApply lifecycle. Instance parameters are not a graph replacement. The
domain supplies isolated proposed state, native settle and instance snapshots;
the common checkpoint, durable package and FinalReadback boundary own recovery.

Material Layer and Material Layer Blend share the Material Function expression owner.
The three Function Instance classes use Reflection profiles and reuse the Material
Instance parameter codec; their native adapter owns parent/parameter synchronization.
Material Parameter Collection uses the Reflection profile, shared keyed desired-state
for named scalar/vector entries and shared Details/ObjectApply for each entry's settings.
These are consumers of the existing public semantic workspace and package lifecycle.

Material Attribute Layers expressions and Material Instances share
`CaptureLayerStack`/`PrepareLayerStack`. The shared ordered-item resolver binds
readable exported rows to the captured native slots; the material adapter preserves
surviving layer identities and applies UE's native add/remove/move operations.
Layer and blend references use the existing reflected value codec. Native tree
parentage is reconstructed from readable row references. Derived runtime graph
caches are invalidated when preparing a changed stack; they are not authoring data.
The Instance adapter additionally reconciles layer parameter indices and controls
the local stack override through UE's static-parameter update lifecycle.
Changing a linked row's name or function references uses native
`UnlinkLayerFromParent`, so parent resolution preserves the authored local row.

The internal contracts below describe provider data, not files that a caller should
construct. Current public authoring edits only the ordinary exported workspace.

## Asset Apply

This section describes the resolved provider document produced by the common graph projection. Ordinary workspace edits use the exported graph's `nodes` and `connections`; Custom input names use `nodes.<alias>.custom_inputs` or an edge targeting `<CustomAlias>.<InputName>`. The graph path manages the reflected `Inputs` property and creates new named inputs before connecting them.

Asset identity in the resolved provider document is `target.asset_path`. Create requires `target.asset_kind` with `material` or `material_function`; update preserves omitted fields; create obtains omitted values from the registered UE factory default provider. Graph input is `provider_extension.graph`, function metadata is `provider_extension.function_settings`, and ordinary UObject fields use `properties[]` with the shared `property_path + value` contract.

Full graph replacement resolves every expression from the supplied class path, catalog identity, or material function path. Multiple supplied identities must resolve to the same class/function. Expression GUIDs are validated before mutation and remain stable through apply/readback. Node properties use shared Object Apply and PropertyValueCodec; the Material domain does not implement scalar, struct, container, or reference codecs.

Ordinary node candidates reach the shared selector validator as the original object, including unknown fields and invalid value types. The material adapter does not reduce them to `name` and `select` before validation. Independent node, property, connection and root-input errors are collected during preparation against the transient owner; checks depending on invalid nodes are marked `unchecked`. Any such error prevents persistent mutation. Actual apply failures still stop and use the shared rollback path.

Expression properties emitted by Export may contain readback annotations in addition to the writable descriptor core. Before full-graph replay, Material projects each exported descriptor through the shared `ProjectDescriptorForApply` contract and applies only the canonical writable fields. Projection or property-apply failures preserve the original structured error detail, so an Export workspace cannot fail opaquely after its descriptor has already passed shared parsing.

## Graph Mutation Apply

`provider_extension.mutation_kind` selects one of:

- `expression`: `create`, `update`, or `remove`.
- `connection`: `create`, `update`, or `upsert` using `from_expression_*` and `to_expression_*` selectors.
- `root_input`: `create`, `update`, or `upsert` using `expression_*` and `material_property`.

Expression selectors may use GUID, name, or index. If multiple selectors are supplied, all must identify the same expression; ambiguity or conflict fails preflight. `expression` create builds a scratch node from the actual UE expression class and applies optional structured properties to that seed, so omitted values retain UE class defaults.

Public authoring uses `content_graph.export_workspace` followed by `content_graph.import_workspace` for Material, Material Function and Material Instance targets. The HJSON contract owns the structured property `value`; old fine-grained HTTP, folder commands and single-document Instance parameter arrays are not public authoring entry points.

## Transaction And Removal

Existing asset updates select the common `ReplaceResidentPackage` readback policy.
Materials, instances and functions may remain referenced by other loaded assets;
native package replacement reloads their published state and updates those
referencers. The common transaction uses the same policy for checkpoint recovery.

Graph replacement and incremental mutations retain prior graph/input state until snapshot verification succeeds. Removed expressions are detached first and marked for garbage collection only at commit.

Asset removal uses the shared `FAssetRemovalStage`: the existing UObject is temporarily renamed inside its original package so the requested identity is absent during verification. Commit performs the physical delete; rollback restores the same UObject and original identity. Material and Niagara share this mechanism, avoiding same-name object recreation over a still-referenced deleted object.

## Verification
