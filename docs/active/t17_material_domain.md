# T17 Material HJSON domain

## Missing-root initialization

Material and MaterialFunction can be created with the complete domain document
submitted through `client.content_graph.import_inline(target, document)`.
For `/Game/Example/M_New.M_New`:

```hjson
schema: "uai.material.edit/1"
root: {
  type: "/Script/Engine.Material"
  create: {
    asset_name: "M_New"
    package_path: "/Game/Example"
  }
}
```

For MaterialFunction, use `/Script/Engine.MaterialFunction` and the matching
destination name. The domain derives `material_kind` from the root type.
MaterialInstanceConstant uses `/Script/Engine.MaterialInstanceConstant` and
also requires `parent_material_ref` in `root.create`, pointing to its existing
parent Material or Material Instance. This native creation route requires the
name and package fields to agree with the independent destination argument.

Layer and Blend do not have this inline creation branch; use the public
`asset.object.create` factory candidate instead. A registered readable root
does not by itself establish a missing-root creation route. Export a created
asset before editing its ordinary workspace.


`material-domain-provider` is the sole `content_graph.*` domain provider for
`/Script/Engine.Material`, `/Script/Engine.MaterialFunction`, its Material Layer
and Material Layer Blend subclasses, and `/Script/Engine.MaterialInstanceConstant`.
Function Instance, Layer Instance, Layer Blend Instance and Material Parameter
Collection use the shared Reflection profile owner through the same public workspace.
The internal Material import schema is
`uai.material.edit/1`.

`material-domain-owner` also owns the only active Material expression
candidate public command, `material.expression.candidates`. It queries the
real Material Graph action schema for one persisted Material or Material
Function. The public creation context is carried separately as readable
`graph_name` and, for pin-drag menus, readable `pin_name`. The result is the
complete matching name list plus one response-level `scope`, deduplicated by original readable name.
Use that name unchanged in HJSON. A same-name conflict is resolved in the current
graph context and, if still ambiguous, returns `candidate_ambiguous` with the
smallest readable `choose` set. Retry the same name with the selected `select`;
do not append a scope to the name. Machine identities and the old paging
envelope are not exposed. The inactive legacy
`reference.catalog_search` binding is neither an ingress nor a fallback for
this domain.

`describe` resolves the same target/graph/pin context as discovery and reports
`description.creation.supported` for ordinary workspace expression creation.
An invalid pin context is reported, not silently replaced by a background menu.

Use `client.material.expression_candidates(...)` for discovery. Export with
`workspace = client.content_graph.export_workspace(target, name="material")`,
check `workspace.result.ok`, edit the ordinary files under `workspace.path`, then
call `workspace.import_changes()` and check the returned result. Use
`client.query.command_search(...)` and `client.query.command_usage(...)` to
discover and inspect live contracts; `ueagentinterfacecmd` exposes the same discovery
path. No task-specific helper or tutorial-owned registry is authoritative.

Material, Material Function and Material Instance updates use the semantic workspace
emitted by `content_graph.export_workspace`. Instance parameters are exported as
ordinary parameter items, not graph nodes. The common graph projection/resolver owns readable node aliases,
connections and desired-state interpretation. The Material authoring provider
adapts that resolved graph to native expressions, compile, persistence and
readback, while the shared HJSON lifecycle owns revision and resource
coordination.

Graph node positions use the shared `pos: {x, y}` workspace field, including
the fixed `MaterialOutput` node. Its native storage is `UMaterial::EditorX/EditorY`,
whereas expression nodes use their expression editor coordinates. Both consume
the same graph projection and numeric conversion; the Material adapter synchronizes
an existing editor root node and includes its position in rollback and persisted
readback. Material Function/Layer outputs remain ordinary expression nodes.

Material's state accessor inherits the shared semantic registry's write capability.
Its graph and native domain documents remain writable; export identity/expression/
edge metadata keeps its registered read-only capability. Do not initialize every
state value as explicitly read-only: that would override the graph contract before
the native graph owner receives ordinary workspace edits. A real per-instance
read-only constraint may still be declared through the existing optional override.

Edit root and expression properties in their exported `properties.hjson`
category groups. Both paths use common compact value completion, including
new container entries with omitted members initialized to native defaults.
For example, a Custom expression's `AdditionalDefines` can be extended while
changing a root property such as `TwoSided` in the same import. Existing
expressions with unchanged identity and type retain their unedited native
state, positions and Custom input names; resolved connections are applied after
property and interface updates. Native identity and type bindings stay inside
the request and do not become fields callers must copy into the workspace.

Custom expression inputs belong to the graph interface. In an ordinary graph,
edit `nodes.<alias>.custom_inputs` with input names, or connect an edge to
`<CustomAlias>.<InputName>`; the Material provider creates that named input before
applying connections. The reflected `Inputs` property is managed by this graph
path, so its property descriptor reports it as unavailable.

Semantic graph updates perform persisted native package replacement and final readback before
the shared transaction commits. The domain supplies native release/rebinding
and separate before-state read requests, because after-state requests may refer
to newly created expressions. Late failure restores the shared package checkpoint,
the original disk artifacts and dirty state, then compares the actual recovered
graph and expression properties. The domain does not run a second post-commit
save/reload finalizer.
Material updates use the existing shared `ReplaceResidentPackage` policy so loaded
material, instance and function referencers are rebound to the published objects.

`UAI.Root.T85.Material.NativeActionVertical` verifies the public Material graph
success path and an apply-seam final-readback rejection after graph replacement:
original nodes, expression value, connection, dirty state and disk bytes recover.
This covers the tested Material update path; it does not establish every
Material Function or Material Instance lifecycle.
`UAI.Root.T85.Material.PublicSelection` verifies a real `Smooth Step` name
collision through public discovery, the returned `form: expression` selection,
ordinary workspace import and independent package reload. It checks the native
expression type/defaults, existing state and re-export; ambiguous, invalid,
unknown and scoped-name attempts preserve the original objects, dirty memory
and disk bytes before retry. This does not establish the function-form creation
or the named Python SDK selection path.
`UAI.Root.T85.Material.PropertyWorkspace` additionally verifies the ordinary
root/property container edits above, complete unedited expression state,
existing connections, independent package reload and re-export.
`UAI.Root.T85.Material.PropertyRecovery` verifies the semantic preservation
strategy after retained expressions change and a node is deleted: rejection
after staging and rejection after persisted reload both restore the original
expression properties, root values/connections and dirty state, with original
disk bytes checked separately.

`UAI.Root.T208.MaterialFunction.Workspace` exercises an independent persisted
Material Function through ordinary workspace node addition, reflected property
editing and FunctionOutput connections. It checks invalid-pin rejection,
independent package unload/reload, preserved expression/output identities and
unrelated state, re-export, and the native transient Function output preview's
shader compilation with an RHI. Its native test fixture is deleted by default,
with the exact package file checked absent. The separate
`UAI.Root.T208.MaterialFunction.PrepareSdkFixture` automation entry prepares only
the named SDK test fixture, refuses an existing asset and records its readable
path under the task temporary directory; it is not a public asset-creation path.

The older single-document `root.graph` and sparse `root.edits` graph mutation
shapes and the single-document Instance `parameters` array are no longer write paths.
Existing Material, Material Function and Material Instance updates require the exported
semantic workspace and otherwise fail with `material_domain_semantic_workspace_required`.
Asset creation retains its initialization contract. Unsupported
root types and create/edit combinations without an atomic native contract fail
before mutation.

Instance parameter items expose scalar, vector, double-vector, texture,
texture-collection, font, runtime-virtual-texture, sparse-volume-texture,
static-switch and static-component-mask effective values and `authored_override`. Edit `value`; when authoring
an inherited value, also set `authored_override: true`. Parameter name, kind,
association and index identify the existing native parameter and cannot be renamed.
Unedited inherited values and unrelated local overrides are preserved, including
local overrides no longer declared by the current parent. An authored texture can
be a Texture reference or explicit null. Set `authored_override: false` to remove
the local override and resolve the parent value; this differs from a true override
with null value. Root properties use the same ordinary categories and transaction.
Parameter values and definitions come from the real native parameter value properties
through the shared reflection codec and ordinary authoring projection. Vectors use
the ordinary four-component array; texture references use the common reference
contract. The domain owns parameter identity, override intent and native setters.
The item's read-only `definition` reference locates its field schema in the shared
definitions catalog; it is separate from editable `values` and parameter identity.
Use a fresh export after import to inspect effective values and override state.

Material Attribute Layers expressions publish their default `layer_stack` in the
ordinary graph. Material Instances publish the effective stack with an explicit
`authored_override` flag. Both consume the same native layer-stack adapter, shared
ordered-item binding and reflected reference codec; `DefaultLayers` and native
parameter arrays are excluded from competing ordinary property writes. The
[SDK tutorial](../../../../UeAgentInterfaceCMD/docs/active/t17_material_domain.md)
describes row bindings, tree parents, insertion, removal and inheritance.

Function Instance, Material Layer Instance and Material Layer Blend Instance expose
`Parent` in ordinary property categories and the same parameter identity/value/override
fields. They inherit expressions from the parent function; they do not have an
independent expression graph. Function, Layer and Blend metadata such as `Description`
uses ordinary root properties rather than a second function-settings write path.

Material Parameter Collection exposes `parameters.value.scalars` and `vectors` as
maps keyed by the current parameter name. Each entry has `settings` containing the
ordinary native `ParameterName` and `DefaultValue` descriptors. Add an entry with
`settings: {}` to use native defaults, edit the exported values, or remove an entry
from the map to delete it. Rename an existing entry through its `ParameterName`
value; the native identity survives and a fresh export uses the new map key.
The map-key resolver, property decoding and transaction are shared HJSON facilities.

`UAI.Root.T208.MaterialInstance.Workspace` covers ordinary scalar, vector, texture
and static-switch edits, root-property composition, independent re-export, non-target
override/inheritance preservation, invalid texture and the historical clear rejection,
and FinalReadback rejection after persisted parameter/root changes with confirmed
checkpoint recovery. It also checks exact native and captured static-switch
identities and removal of its two fixture packages from disk. This does not
establish other material parameter types. The clear-rejection expectation predates
the current supported clear operation. Subsequent public SDK authoring covers
clear-to-parent, explicit null texture, the six additional parameter kinds, compact
Font/Mask edits and all three function-instance parent/override paths; see
`tmp/TROOT-UAI-208-material-routing-instance/` and
`tmp/TROOT-UAI-208-material-function-instance-m4/` in the development project.

Add the returned name to a node in the ordinary exported graph document:

```hjson
nodes: {
  Multiply1: {
    name: Multiply
    candidate: { name: "Multiply" }
  }
}
```

`candidate` contains the returned readable `name` and, only when necessary,
the short readable semantic `select` fields needed to disambiguate. Select values come from the current
ambiguity response; an invalidated selection returns `candidate_selector_invalid`
with the current choices when ambiguity remains. If the name is now unique,
retry it without `select`. Keep existing nodes and connections in the exported
document. Use the ordinary graph's own context; do not copy internal class paths,
candidate binding context, or an internal `value` wrapper into it. Layout and
properties remain outside the candidate. This graph update is not a `root.edits`
mutation. Missing, ambiguous, context-mismatched, or unsupported actions
fail before mutation; callers do not copy a complete search row, machine
identity, or revision into HJSON.

Exported parameter nodes use their native `ParameterName` as the shared graph
alias (for example `Radius`), while `name: ScalarParameter` retains the expression
type. Function inputs/outputs use their native connector names by the same rule.
The property remains editable in its ordinary property shard; no second label
needs synchronizing. Renaming it refreshes the alias and references while the
expression's native identity remains unchanged. Duplicate names receive suffixes.

Queries capture the actual Material owner through the common content-graph
read environment and return its stable revision, items, relations, parameters,
diagnostics, and compile state. Imports require the observed revision, invoke
the registered Material authoring owner, and independently recapture the
persisted owner. Public factory candidates and `asset.object_create` support the
nine persistent asset types listed above. Function, Layer and Blend use their native
factories and share the existing expression graph owner. Function Instance and its
two subclasses share the Material Instance parameter codec, while their native
parent and parameter-update lifecycle remains owned by the function-instance adapter.

`content_graph.items` uses the same Material authoring capture and composition
as workspace export. Node `name` values are the ordinary graph aliases, including
repeated expression types. Nodes expose `node_type`, position, and, for function
calls, the actual `material_function` asset reference. Request `pin_interface`
explicitly to read the current input/output interface; `pins[].alias` is the
same Pin alias used in exported HJSON connections, including unnamed and repeated
native pins. It is omitted from the default compact result. Candidate binding context remains private.
These are readback fields, not an alternative graph mutation document.

`content_graph.export_workspace` publishes the registered Material authoring
workspace for Material, Material Function and Material Instance targets. The
same semantic directory is the only graph update surface for existing Material
and Material Function assets; it supports the exported root properties and
readable graph units in one import. All model-visible HJSON uses readable
aliases and names; GUIDs, hashes, candidate ids, and class references remain
machine-side implementation details.
