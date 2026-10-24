# T14 Blueprint Surfaces

To create a generic function graph in an existing asset, edit the always-exported
`graph_declarations` map with a readable name and `{ graph_kind: "function_graph" }`.
Import and read the refreshed workspace before querying `client.blueprint.node_candidates(...)`
for that new graph and adding its returned node name in a second ordinary import.
Export again only if the workspace was not refreshed. Graph-body
name/kind are read-only references; nodes, pins, edges and layout stay in the
body. Omit a function or macro declaration to remove that graph; leave its old,
unedited body files in the workspace. Removing a declaration while editing its
body is rejected before writing. Event and animation-specific graph protections
remain native. Changing a map key means remove plus create, not identity-preserving
rename; kind conversion is unsupported. To rename an existing ordinary Function or Macro,
keep its declaration key and add the optional business name, for example
`ComputeStatus: { graph_kind: "function_graph", name: "EvaluateStatus" }`.
The existing key selects the captured graph identity. The server applies native
reference updates to callers and local scope in the same Blueprint, then compiles,
saves and reloads within the existing atomic lifecycle. Do not edit body headers
or hidden files to repeat the rename. A fresh export uses the new readable key
and omits its redundant name. Native name conflicts and actual effects on external
Blueprint callers, child overrides or child members are rejected before writing.
When the host already enables MVVM, its native binding-reference and migration
effects are checked through an optional adapter; rename does not enable MVVM.
An unrelated dependency alone does not reject the rename. Ordinary K2 functions,
macros and members in Widget/Animation Blueprints use this same native rename path.
Protected or animation-specific graphs and cross-asset rename transactions remain
outside it. Macro instances keep their graph identity and signature.
MacroLibrary rename preserves native old-name redirectors for saved external imports.
Its save and checkpoint readback use the existing resident-package replacement policy:
native redirectors moved to Transient can otherwise keep the old package alive.
The same macro can reclaim its old name, including late-failure restoration.
Loaded external instances retain their object identities, unsaved edits and disk bytes
during rename and late restoration. A redirector keeps its old name bound to its
original graph. Creating a new graph with that name is rejected. A rename may reclaim
it only when the redirector points to that exact graph; another or unresolved target
rejects before writing with `blueprint_graph_name_identity_conflict`.
Taking another graph's old name requires explicit reference migration, which is not
supported. This name-identity conflict does not claim an external consumer was found.
Native identities remain server-owned; no extra files or manifest edits are needed.

For an existing dispatcher in an ordinary Blueprint, keep its `delegates` key and
add an optional `name` to rename it. Export omits an unchanged, repeated name.
The key retains the captured member identity; changing the key remains removal
and creation. Edit the dispatcher signature in that same declaration. Its native
signature-graph name and same-Blueprint Bind/Call references follow the rename;
do not edit hidden identities, `new_name`, or a second graph-name declaration.
The retired delegate `new_name` field is explicitly rejected before writing.
The same member document keeps its `variables` and `interfaces` collections;
editing one preserves the others. An omitted collection keeps its captured state; an empty
collection explicitly requests removal of its items. The dispatcher's derived
item state is hidden, leaving one editable name/signature declaration.
Name conflicts and actual native writes to external Blueprints reject before
the source is modified. Unrelated dependencies remain allowed. A dispatcher with
a RepNotify callback is ineligible because the native rename requires interactive
confirmation. Ordinary dispatcher rename preserves member, graph, node and Pin
identities, signature and connections through save/reload and late-failure
recovery. Widget and AnimBlueprint members use the same eligibility checks.

Rename an ordinary Blueprint variable by setting the optional `name` on its
exported `variables` entry. Keep the map key: captured member identity selects
the existing variable, and omitting `name` keeps its captured name. Variables and
dispatchers share this name/identity path; the retired variable `new_name` is
explicitly rejected before writing. Native rename preserves the member GUID,
copies the current CDO default value, and updates same-Blueprint Get/Set references.
Save/reload and late-failure restoration preserve defaults, identities, Pins and
connections. Actual dependent and child-conflict writes reject before source
modification; RepNotify-associated variables reject because the native operation
requires interactive confirmation.

Change a variable's exported `type` while keeping its map key and captured GUID.
The adapter applies native member flags and K2 reconstruction without an
interactive warning. Native reconstruction decides which connections survive,
migrate, disconnect or remain on orphan Pins; unchanged exported connections
are context and are not replayed over that result. Submit explicit connection
edits only when a different connection is intended. Native compile errors
remain failures, and the resulting member type and GUID are checked.

Loaded child Blueprints affected by native reconstruction participate in the
same capture, compilation, save/readback and failure restoration. Native
Set/Map type changes clear unsupported replication settings. This does not
rewrite arbitrary unloaded cross-asset consumers. Export again to obtain the
settled native graph and updated references.

Rename an existing Custom Event in an ordinary Blueprint by editing its exported
`event_name`. Keep the node key; its captured NodeGuid selects the original node.
The definition node remains the sole signature owner, and an omitted signature
keeps its current value. Native `OnRenameNode` and the existing compile phase
resolve same-Blueprint Call and CreateDelegate references by native function identity.
Unchanged caller names in a whole-body edit follow the settled native reference;
explicit caller retargeting retains its requested value. No `new_name` or hidden
identity edits are needed. Native event-name validation applies, including its
parent-function restrictions. Loaded external Call references that compilation
would change reject before writing, including changes that do not emit Modify
or dirty notifications. Persistent rename also rejects exact external CreateDelegate
references: native package replacement refreshes their cached names, even though
compilation alone does not. Unrelated dependencies remain allowed. Ordinary
Blueprint saves use the shared resident-package replacement policy so retained
references cannot prevent genuine source reload or checkpoint restoration.
Custom Event rename does not invoke
the graph/member rename broadcast or its MVVM migration path. Graph/node/Pin
identities, signature and links are preserved through save/reload and late-failure
restoration. Editable Custom Events in Widget/AnimBlueprint sources use the same
path; cross-asset reference migration remains outside this operation.

Omitting a member or a deletable Function/Macro declaration invokes the native
Blueprint removal lifecycle. Native ownership determines which derived graphs,
member nodes and connections disappear and which unresolved callers remain for
compiler diagnostics. Unchanged exported context must not recreate removed
objects or require manual removal of every dependent node.

Explicit caller and connection removals can still accompany producer removal in
the same workspace. The plan applies the complete removal set before compiling,
saving and reloading. Editing the body of a graph that is also removed is a
conflicting request. Native non-deletable graphs retain their deletion rules.

Member type changes reconstruct native consumers, including loaded child
Blueprints, and persist the affected packages through the common transaction.
Do not manually load unrelated package referencers just to satisfy an obsolete
preflight error. This does not promise automatic rewriting of arbitrary soft or
dynamic references; unresolved native compiler errors are reported and failed
transactions restore their captured state.


`workspace.import_changes()` imports an existing Blueprint directory workspace.
`client.content_graph.import_inline(...)` and `import_file(...)` initialize missing
targets. These SDK entries share the public `content_graph.import_workspace`
command; the SDK supplies its source carrier and directory concurrency context.
No Blueprint-specific apply command is public. Shared
`content_graph.*` reads and `content_graph.export_workspace` are the public
inspection and export paths.

When the generated method is not already known, use
`client.query.command_search(...)` and `client.query.command_usage(...)`;
`ueagentinterfacecmd` exposes the same discovery path.

The accepted entry is `uai.blueprint.edit/1` with exact root type
`/Script/Engine.Blueprint`. The Blueprint Surfaces provider uniquely owns
ordinary Blueprint settings, parent, interfaces, class defaults, members,
component hierarchy, and managed graphs, nodes, pins, and edges. T13 owns
carrier loading and recursive initialization. T14 consumes only the T36
resolved-document and lifecycle seam; it does not reopen source files, build a
second result envelope, or claim success without an explicit persistence
receipt and independent readback.

For a missing Actor Blueprint, pass the following complete document to
`client.content_graph.import_inline("/Game/Example/BP_New.BP_New", document)`:

```hjson
schema: "uai.blueprint.edit/1"
root: {
  type: "/Script/Engine.Blueprint"
  document: {
    operation: "create"
    target: { asset_path: "/Game/Example/BP_New", asset_kind: "actor_blueprint" }
    provider_extension: { parent_class: "/Script/Engine.Actor" }
  }
}
```

The outer target is the full object reference; `document.target.asset_path` is
its matching package. Other Blueprint parents use their actual class reference;
ordinary non-Actor Blueprint creation uses `asset_kind: "blueprint"`.

Ordinary root settings are edited in the exported `properties.hjson`, under
their existing category and property-name key, using the displayed `type` and
`value`. A root-only edit is valid; it does not require a graph or Component
change. For example, changing `bRunConstructionScriptOnDrag.value` from `true`
to `false` is verified through public import, compilation, independent disk
reload, and re-export while preserving the existing parent and graph structure.
Importing an unchanged workspace returns `hjson_import_no_explicit_changes`.

For semantic workspace updates, final readback runs inside the shared
transaction. Recovery is verified both after staging and after persisted
reload: an import failure restores the previous dirty Blueprint settings and
captured graph/Pin state, while restoring the original disk contents separately.
Missing-target creation also keeps final readback inside the shared transaction.
The verified Actor Blueprint initialization path restores object, asset-registry,
and disk absence after either staging or persisted-readback failure. Native
non-forced deletion releases the failed creation through the editor lifecycle.

## Candidate-bound node and Component creation

Use `blueprint.node.candidates` and `blueprint.component.candidates` when adding
their supported objects. Their exported file shapes are intentionally distinct.
For an ordinary component-tree, put one direct top-level readable `name` selector
into the new stable-map row:

```hjson
components: {
  PointLight1: {
    name: "Point Light"
    parent: DefaultSceneRoot
  }
}
```

The Component stable-map key owns the instance name. Do not write a retired
`provider_extension`, explicit `operation`, or nested `candidate`; the keyed
projection restores candidate/create internally. Known writable initial values
may be supplied in the new row's `properties` descriptor array through the shared
property payload owner. If fields are unknown, create the minimal row and read
the refreshed workspace. Subsequent edits use the persisted Component's property
units. Blueprint graph nodes continue to follow their current exported graph
file and may use its declared candidate envelope; do not copy the flattened
Component row shape into graphs. The provider
re-enumerates the same Add Component registry or Blueprint Action Menu before
the first persistent write and invokes the same native action. Optional richer
fields are assertions only. Unknown, ambiguous, stale, mismatched-context, or
conflicting candidates fail before mutation and leave the public asset revision
unchanged; there is no class/spawner fallback.
Blueprint node queries retain readable name-context entries for disambiguation,
but an unreadable action outside the actual query matches does not fail the
query. A matched action with an invalid public name still returns its specific
diagnostic. Native navigation actions remain outside the creation candidates;
catalog enumeration and identity failures remain explicit failures.

Existing-asset edits use the target and exported workspace; its hidden manifest
owns the baseline. Inline/single-file creation uses the server-owned missing
expectation. Callers do not supply `expected_revision`.

For graph editing, follow [the public graph interface, defaults and connections
contract](../hjson-authoring/00_DirectHjsonAuthoring.md#图接口默认值与连接).
`connections` is an array of `{ from: "NodeAlias.OutputPin", to: "NodeAlias.InputPin" }`.
Use the graph's current node aliases and its `interface_ref` for real Pin names
and directions. Omitting the array keeps existing links; an explicit array is
the complete desired link set. Node positions use top-level `pos: {x, y}`.
After import, locate nodes through the refreshed workspace, since their readable
aliases may be normalized; an input alias is not a persistent node identity.
Explicit compile/report access follows [the compile workflow](t47_blueprint_command_migration.md#consolidated-blueprint-workflows).

For an existing local dispatcher and a matching Custom Event, query its delegate
output context with `blueprint.node_candidates(target, "EventGraph",
query="StatusChanged", node_name="HandleStatus", pin_name="OutputDelegate")`.
Use the returned Add Delegate or Remove Delegate name in the ordinary graph:

```hjson
BindStatus: {
  name: "Status Changed [K2Node Add Delegate]"
  candidate_context: { node: "HandleStatus1", pin: "OutputDelegate" }
  pos: { x: 512, y: 320 }
}
```

The context uses the existing exported node/Pin aliases. Native action creation
connects the delegate Pin; it also connects the event's execution output when
that output is unconnected. To place Remove after an existing Bind, keep the
existing connections and add the ordinary `BindAlias.then -> RemoveAlias.execute`
connection. After a successful workspace refresh, read that workspace for the persisted
aliases and interfaces; export again only when the result does not provide a refreshed workspace.
Background discovery can also return these actions without a node/Pin context.
Query the dispatcher name for Add; a complete returned name such as
`Status Changed [K2Node Remove Delegate]`, or `StatusChanged Remove`, narrows
the background query to Remove. Use that
name without `candidate_context`, and add the ordinary delegate and execution
connections explicitly. Both background and Pin-context Add/Remove chains are
verified with a local bool/int dispatcher signature, including native references
and persistent reload. Assign remains outside this validation.

### Shared Blueprint member variable creation

Within the exported `member_collection` value, `variables` is a keyed map.
Keep existing entries and add a readable variable name with a `type`; the server
creates the native member identity. An empty map does not require an existing
variable template. For example:

```hjson
variables: {
  Amount: { type: { pin_category: "float" } }
  Enabled: { type: { pin_category: "bool" } }
  Count: { type: { pin_category: "int" } }
}
```

The shared Blueprint pin-type parser owns this declaration, including Widget
Blueprint member variables. Common type forms are:

| Type | Declaration |
|---|---|
| Boolean / integer / int64 | `pin_category: "bool"`, `"int"`, or `"int64"` |
| Float | `pin_category: "float"` |
| Double precision | `pin_category: "real", pin_subcategory: "double"` |
| Name / string / text | `pin_category: "name"`, `"string"`, or `"text"` |
| Object reference | `pin_category: "object", pin_subcategory_object: "<reflected class path>"` |
| Struct | `pin_category: "struct", pin_subcategory_object: "<reflected struct path>"` |
| Array / set | Add `container_type: "array"` or `"set"` to the element type |
| Map | Use the key type with `container_type: "map"` and `value_type: { pin_category: "float" }` (or another non-container value type) |

Use explicit `pin_subcategory: double` for double precision: a real category
without a subcategory defaults to float. Obtain reflected paths from current
public discovery rather than guessing them. After workspace refresh, edit the
exported generated-class default property to set an existing variable's value;
member type declarations and CDO values have different owners. Preserve exported
type fields when editing an existing member. This input contract does not by
itself establish runtime validation of every reflected type or container.

A Blueprint user variable can use the reflected `RuntimeVectorCurve` struct
type (`pin_category: "/Script/Engine.RuntimeVectorCurve"`) in the ordinary
member collection. After importing the new member, read the refreshed workspace
and edit its CDO property row. Its referenced RuntimeVectorCurve definition supplies rich-key
enums/defaults and embedded XYZ or external UCurveVector templates. Each channel
owns its defaults and extrapolation. Existing variables' defaults belong to CDO
properties, rather than a second editable default in the member declaration.
The generic key coordinate `time` has the meaning chosen by the Blueprint's
consumer; this value representation does not assign seconds or spatial units.
External selection retains inactive embedded curves. This does not establish
standalone UCurveVector asset HJSON support.

Success is terminal only after native
compile/settle, save, package unload/reload, and independent readback of all
intended surfaces. Unsupported roots or creation roles, identity mismatch,
duplicate or missing entry documents, stale revisions, unresolved type
identities, compile or persistence failures are typed terminal failures with
no retired-owner fallback or residue.
