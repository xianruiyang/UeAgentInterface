# T20 AI Behavior HJSON domain

`ai-behavior-domain-owner` is the sole public owner for
`ai.behavior.candidates` and the `uai.ai_behavior.edit/1` domain. Discover candidates
with `client.ai.behavior_candidates(...)`. Obtain
`workspace = client.content_graph.export_workspace(...)`, edit its ordinary
HJSON files and call `workspace.import_changes()`. For unknown command names or parameter shapes, follow [public command discovery](../../../../UeAgentInterfaceCMD/docs/PYTHON_SDK_AND_CLI.md#command-discovery).

The shared source loader owns inline, single-file, and directory carriers. T20
consumes only the normalized `source.kind=resolved` document set and does not
accept legacy top-level `carrier` or `hjson_document` fields.

T13 remains the sole common recursive initialization authority. T20 registers
only AI Behavior root and owned-entry initialization contracts and consumes
that shared seam; it does not create another recursive dispatcher.

## Supported roots and native owners

| Root type | Create command | Mutation command |
| --- | --- | --- |
| `/Script/AIModule.BehaviorTree` | `ai.behavior_tree.asset.create` | `ai.behavior_tree.graph.apply` |
| `/Script/AIModule.BlackboardData` | `ai.blackboard.asset.create` | `ai.blackboard.schema.apply` |
| `/Script/AIModule.EnvQuery` | `ai.eqs.asset.create` | `ai.eqs.graph.apply` |
| `/Script/StateTreeModule.StateTree` | `ai.state_tree.asset.create` | `ai.state_tree.hierarchy.apply` |
| `/Script/SmartObjectsModule.SmartObjectDefinition` | `ai.smart_object_definition.asset.create` | `ai.smart_object_definition.apply` |

T20 owns root classification, target/revision validation, candidate binding,
and lifecycle completion. It forwards one normalized request to the existing AI
command owner and does not duplicate graph planning, UE construction,
compilation/validation, persistence, reload, readback, or cleanup mechanics.

## Root lifecycle and edit shape

For a missing BehaviorTree destination, submit the complete domain document
with `client.content_graph.import_inline(target, document)`:

```hjson
schema: "uai.ai_behavior.edit/1"
root: {
  type: "/Script/AIModule.BehaviorTree"
  create: {}
}
```

The same envelope applies to the five root types in the table above. The public
target supplies the asset name and package path; conflicting declarations fail
before write. Creation contains `root.create` without a root `operation`.
Export the created root, then edit its ordinary workspace and import changes.
The hidden workspace manifest owns identity and concurrency state; the revision
used by the lifecycle is server-owned and is not part of the public success result.

Existing-root import uses the exported workspace; its hidden manifest owns the
current fence. The operation params use each native command's stable item/relation/key/slot shape:
BehaviorTree nodes and hierarchy, Blackboard entries, EQS options and
generators, StateTree states and node placement, or SmartObject slots and
behavior definitions.

## SmartObject workspace

SmartObject uses the common workspace export/import path. Its ordinary
`smart_object_items` collection exposes readable
names and references for settings, parameters, slots, owned behaviors, definition
data and bindings. Item properties use categorized files and the shared value
codec; native IDs and binding descriptors stay outside the ordinary editing face.

Existing slot values, owned behavior properties and parameter values can be
edited. Parameter changes run native binding propagation before persistence;
Reflection owns checkpoint, save/reload, independent comparison and failure
recovery. Ordinary edits do not invoke the complete UObject loading lifecycle.
Slots, owned behaviors and definition data support creation, deletion, array
ordering and readable parent references. Add a named row with top-level `name`
from the candidate query, an optional `select`, `parent`, `index` and initial
`categories`. The collection key names a new slot. Remove a row to delete it;
unchanged owned children leave with it. An edited child must retain a surviving
parent; contradictory edits are diagnosed rather than discarded. Slot annotations require a slot
parent. Existing owned behaviors retain their object properties when reparented.

A SlotLinkAnnotation expresses its target with `linked_slot` on the definition-data
row. Use an exported slot name, the name of a slot created in the same import, or
an empty string/null to clear it. Native slot identity and the derived runtime
index remain server-owned. Reordering slots preserves the target. When deleting a
referenced target, an unchanged link follows native handling: its identity remains
captured and its runtime index becomes unresolved. The refreshed workspace shows
a readable unresolved-reference handle; it does not expose the native ID. New or
changed links still require a surviving target. The former empty `data.LinkedSlot`
property is absent from ordinary authoring; complete capture retains its native state.

Settings and parameter owners are fixed. Their parent/order cannot change.
Existing item retyping is explicitly rejected.
Parameter values and their schema use the shared PropertyBag `schema`/`fields`
value: both collections must describe the same complete field set. Add or remove
a field in both, or change its schema `kind` with a compatible value. Surviving
names retain their native field identity and non-public metadata. Schema changes
run the same native parameter/binding propagation and persistent comparison.
Bindings support creation, source/target path edits and deletion through the same
collection and Reflection transaction. Query `Binding`, then use the returned
name in a new row with `source`, `target`, `source_path` and `target_path`.
Endpoints are exported item names; paths are relative native property paths.
For example, a float parameter `Size` on `Parameters` can bind to `Offset.X` on
an exported slot. The native binding owner applies the value before save/reload.
Invalid paths, incompatible types, duplicate targets and non-bindable targets
(including EditorOnly properties) fail before mutation for newly requested bindings.
Removing a referenced parameter or slot lets native binding refresh clean unchanged
bindings; import does not replay them afterward. Explicit binding edits must survive
native refresh or produce a diagnostic.
Empty-path whole-struct/context sources remain unsupported. The candidate source
covers loaded native types and intrinsic Slot/Binding items, not the complete
native editor menu.

WorldCondition bindings use the editable condition path, for example
`SelectionPreconditions.EditableConditions[0].Condition.TagQuery` for a slot's
ObjectTagQuery condition. A compatible GameplayTagQuery parameter can be created
in the same import. Native initialization and binding propagation update both
editable and runtime condition values before persistence. The ordinary collection
exports only the picked binding; its retargeted runtime copy remains in complete
capture for verification and recovery. Parameter changes and binding deletion
preserve this distinction. This does not establish gameplay activation of conditions.

The internal GraphCompound slot operation projects into this same complete
document and Reflection transaction. Its old single-slot NativeHook has exited.
ContentGraphRead uses the shared canonical capture and keyed projection, including
property ownership and revisions that change with property values.

## Candidate-to-authoring chain

`ai.behavior.candidates` is read-only and target-sensitive. Its public filters
are `target` and optional text `query`. BehaviorTree also accepts
`parent` (an exported node alias) with `attachment_kind` (`service` or `decorator`).
For an existing CompositeDecorator's internal graph, pass `graph_owner` copied
directly from that graph's exported `owner` value. This context is exclusive with
`parent` and `attachment_kind`. Omitting all context fields selects the
blank-background menu. The BehaviorTree branch enumerates the selected native schema
menu without opening an asset editor and returns readable
names without adding scope prefixes. BehaviorTree candidate resolution uses the
shared exact-name selector; ambiguity returns readable `name/select` choices.
Selection fields describe the native kind, class name or readable source folders/module, and only the
shortest distinguishing fields are returned for correction.

For BehaviorTree, composite, task, service, decorator and CompositeDecorator menu
actions use the native creation action. CompositeDecorator creation also creates
its native default owned graph and Result node. Within that graph, native
AND/OR/NOT and decorator candidates use the same internal schema actions as the
editor menu. Auto Arrange and Add Comment remain search-only; other pin contexts
are not yet covered. Put only the selected readable `candidate.name` and,
when necessary, the short readable semantic `select` fields needed to disambiguate inside the candidate
object. Node identity, position, ownership, and relations remain in the
surrounding edit:

```hjson
nodes: {
  Focus1: {
    candidate: { name: "Default Focus" }
    parent: Wait1
    attachment_kind: service
    order: 0
  }
}
```

`Wait1` must be a parent alias in the same graph; it can be created in the same import.
The query and import use the shared captured alias projection. Services require a native
task or composite context that supports services. Invalid contexts fail before mutation.
The domain resolves the readable selector during normalization, derives or
checks the carrier and payload types, then re-enumerates the same native context
immediately before commit and invokes the same schema action. An unresolved
same-name ambiguity fails closed. BehaviorTree relations use the native schema
connection path; the root must connect to a composite, not directly to a task.

BehaviorTree, Blackboard, EQS, StateTree, and SmartObject discovery share the
same name-list and query-`scope` public result. Their class, struct, signature, support and
revision metadata remain inside the owning context and are resolved again from
the readable selector before authoring. This does not claim that every catalog
is equivalent to its native editor menu; each owner still needs independent
coverage verification.

## Blackboard workspace

Blackboard uses the common workspace export/import path. `blackboard_keys` contains
local keys; `parent` selects the parent asset by package path and `inherited_keys`
is a read-only native cache. Each key exposes `index`, `description`, `category`,
`instance_synced`, and the read-only `key_type` label. Keep `index` unchanged to
preserve native key order. KeyType properties use the key's ordinary categorized
property file, including object-key `BaseClass` and typed `DefaultValue`.
An actor target used by `Move To` must have an Actor-compatible `BaseClass`
(for example `/Script/Engine.Actor`); a generic UObject key is not accepted by
the native task and Unreal may select another compatible key.

Create a key by adding a named collection entry with the exact `name` returned by
the candidate query and optional initial `categories`. Blackboard discovery uses the
shared derived-Blueprint asset discovery together with loaded native classes and the
native KeyType picker rules. Query and import resolve the same complete catalog;
use short `select` fields only to resolve a real same-name ambiguity. The map
key is the new Blackboard entry name. Omit an existing entry to delete it. To replace
an existing KeyType, retain the map key and metadata and add `candidate: { name: ... }`
with optional initial `categories`; the new type starts from native defaults.
Editing `key_type` itself is rejected rather than silently changing a display label.
Ordinary property edits use the common changed-property projection and native
property notifications. For example, setting `EnumName` resolves `EnumType` through
Unreal's own callback; an unchanged exported sibling field does not overwrite that
derived value. Initial properties are applied to native defaults before their
completed state is captured.

Parent changes follow Unreal's inherited-key and persistent SelfActor behavior.
Inherited keys are not copied into local Keys. Ordinary import and the existing
GraphCompound `entries` route share the complete Blackboard document and Reflection
checkpoint/save/reload boundary. Blackboard uses native package replacement to
rebind loaded external references, then refreshes derived Blackboard caches.
Unedited child packages are not saved by the parent operation. ContentGraph reads bind KeyType properties to their
keys and derive revision from canonical content, including property values.
`content_graph.items` returns each key's native `name`; use that returned name
unchanged in `items` selections or `properties` owners, including names with
spaces or dots. Property reads consume the canonical type, default and presence
state used by the common workspace.

After a key deletion has been saved and reloaded, a late rejection restores the dirty document
and original disk. Across committed parent changes and late rejection, a loaded child with unsaved
local KeyType properties retains or restores its parent reference, inherited caches, local values,
dirty state and original disk as appropriate. This does not establish
every KeyType or more complex loaded reference graph. Candidate-based creation supports initial
properties, independent export, and item/property reads after Editor restart, including names with
spaces and dots.

## StateTree workspace

StateTree exports its persistent hierarchy, nodes, transitions, parameter groups,
binding sources and bindings through the shared `tree_items` collection. References
use the exported readable keys. Node, instance, instance-object and editor properties
use scoped paths in the common property files. Existing root and state parameters
retain their native descriptor identities; property edits do not invent new IDs.

`content_graph.items` returns readable names for structural items as well as nodes.
Use those names unchanged as `content_graph.properties` owners; scoped fields such
as `state.Description` retain the common canonical type, default and presence
contract. Read-side snapshot validation covers non-node native carrier types too.

The candidate query uses the native StateTree class cache filtered by the target
Schema. `State`, `Transition`, `Binding`, and `Color` are structural candidates; task and condition names
come from the query. Ordinary import supports creating a state and a task beneath
that new state, and resolving a transition to an existing state in the same import.
New node defaults come from native initialization and the same complete capture.
New-item initial properties use `categories` with the common `type`/`value` format
and scoped property paths, for example `Parameter` → `instance.Duration` →
`{ type: "float", value: 6.5 }`. The native temporary item validates these values
before the target transaction; omitted values retain native defaults. This has
been verified for a native Delay task, including invalid-value rejection without
changing the original live tree or disk, followed by corrected import and reload.
Inline `categories` is creation-only. For existing items, edit the exported property
files; adding inline categories to an existing structure row returns
`state_tree_initial_categories_require_create` instead of silently ignoring it.
Removing a task's structure row deletes the task even when its unedited property
companion remains. Node labels own native renaming; the native node `Name` is not
exposed as an independently writable property.

StateTree mutation uses the existing Reflection transaction for complete checkpoint,
apply, save/reload and final readback. The retained GraphCompound hierarchy adapter
projects creation, naming and order operations into that same document. An existing
children-order entry uses `update`; `create` remains reserved for absent identities.
Existing binding rows support editing `source_path` and `target_path`. Only changed
paths are rebuilt using native path resolution on a temporary desired tree; native
compilation checks the result before the target transaction. Unchanged paths retain
their complete captured state. Root-parameter source changes and native Delay target
changes have passed save/reload verification; nonexistent fields are rejected without
changing the original live tree or disk.
Existing bindings also accept readable `source`/`target` references, including a
task created in the same import. An existing state item that is not a bindable data
source/target is rejected. Native compilation rejects incompatible property types
before target mutation. Deleting a bound task requires removing its binding rows
as well so the desired document contains no dangling references.
Use the queried `Binding` candidate name to create a binding row with readable
`source`, `source_path`, `target`, and `target_path`. The target may be a task created
in the same import. Two rows claiming the same native target path are rejected with
`state_tree_document_binding_target_duplicate`; edit the existing row to replace
its binding. Node-to-node creation alongside a parameter binding has passed
save/reload verification.
`state_tree_binding_compile_failed` includes the first native compiler error's
readable item/field names and reason in the import error detail.
Property-function candidates can be created as rows whose `parent` is the consuming
item. A corresponding Binding row references that function as its `source` and its
native output path (for the float Add function, `Result`). Functions are owned by
exactly one output binding; isolated or multiply owned function rows are rejected.
Function input values use common initial `categories`, and input properties can also
receive ordinary bindings. Float Add creation, nondefault inputs, an input binding,
output binding, save/reload and associated deletion have been verified.
Calculated function outputs remain in complete recovery snapshots but are excluded
from ordinary property files using native Output metadata. Function input properties
remain visible; this has passed the same workspace save/reload and deletion checks.
An isolated function, two output bindings owning the same function, and an Output
creation initial value are rejected before target mutation. These cases preserve
the original live document and disk bytes; calculated outputs cannot be initialized
through ordinary `categories`.
Two nested float Add functions have also passed ordinary creation, compilation,
save/reload and associated deletion: the inner `Result` binds to the outer `Right`.
Function ownership must terminate at a non-function consumer. Self-references and
cycles between functions return `state_tree_function_binding_cycle` before mutation;
the rejection leaves the live tree and disk unchanged. The public path supports asset creation,
names-only candidate selection, ordinary task/function/output-binding import and an independent
workspace export. Destructive asset removal continues through the public preview/confirm flow.
Empty internal structure-surface containers and their ordering stay in the hidden
workspace mapping. A public SDK export verifies that only the real tree structure
file appears in the ordinary structure directory; the full import/recovery chain
continues to consume the preserved internal units.
Complete tree documents retain non-editable nested native fields for reconstruction;
ordinary property exports and creation defaults keep their editable-field policy.
Palette entries use the same `tree_items` readable keys and hidden identity mapping.
Their labels can be changed on the row; color values use the common `color.Color`
property. States select an existing entry with `color_ref`; an empty reference retains
the native default reference. The old `editor.Colors` and raw `state.ColorRef` property
paths are replaced by these item/property/reference surfaces.
Palette rename, RGBA editing and selecting the same custom entry from another state
have passed import, save/reload and native identity/value readback. Multi-placement
capture and reconstruction also pass. Public SDK editing and independent export verify
the palette label/value surface. Use the queried `Color` name for a new palette row;
its `categories.Theme` initial value uses `color.Color` with type `linear_color`.
Native defaults supply omitted values and a new identity. A state can reference that
new row in the same import. Removing a referenced color lets the native Colors
notification reset unchanged state references to the default color; explicit new
references still require a surviving palette entry. Selecting the default palette
entry normalizes to the native empty reference. Clearing all palette entries restores
the native default color. Earlier rejection of captured references to removed colors
was an import restriction, not native editor behavior; that restriction is retired.
Existing state-event payload bindings support ordinary source-path editing. The
workspace test changes `Payload.Duration` to `Payload.RandomDeviation` and verifies
the native instance type and original task endpoint after save/reload. An invalid
payload field is rejected without changing the live tree or disk. This does not
cover transition-event bindings. A public SDK check also enables
`state.bHasRequiredEventToEnter` and sets `state.RequiredEventToEnter` in the same
ordinary import, then independently exports the new state-event binding source.
Authoring eligibility retains native property flags and codec support; conditions
are evaluated against the desired values during apply. Disabling the condition
while changing the event descriptor is rejected, with ordinary readback unchanged.
This does not establish same-import creation of a binding to the new event source.
Blueprint task candidates also pass the public names-only query and ordinary
`tree_items` creation path. Their UObject instance properties use the shared
`instance_object.*` category surface. Public checks cover boolean and FText edits,
non-default text initialization of another same-class node, independent instance
values/defaults, and deletion of that sibling while preserving the first node.
For ordinary updates, the transaction resolves FText against the target package
once and reuses the shared resolved descriptors when applying the actual tree;
exact localized identity verification remains enabled. The dedicated
`UAI.Root.T85.StateTree.TextLifecycle` regression rejects after the new text is
saved and reloaded, then checks restoration of the original dirty text and exact
localization identity, package root metadata, complete tree document, and original
disk bytes. The referenced task Blueprint remains unchanged and both test assets
are removed. This update coverage does not establish Reflection create/clone text
identity behavior.
Ordinary workspace edits of existing root and state Float parameters also retain
their native PropertyBag descriptor identities after save/reload, including the
state parameter override and existing bindings. Public SDK checks additionally
cover adding a root Float parameter, editing its value, and removing the last
parameter, with independent workspace readback. An explicit PropertyBag `schema`
defines the desired field inventory: omitted values can retain captured values
only for unchanged descriptors; removed fields are not restored by sparse merging.
Unknown explicitly supplied fields still reach the shared codec's strict validation.
PropertyBag fields also use the shared recursive property preparation. Ordinary
root Text parameter creation and source editing are verified through the public
SDK with independent exports. The StateTree `TextLifecycle` regression includes
a newly created root text parameter and verifies exact dirty text identity,
document, package metadata and original disk restoration after a late rejection
following save/reload. Schema parsing and native descriptor preservation remain
owned by the shared PropertyBag codec; text identity is resolved by the existing
target-package transaction and transferred to the actual object.
Removing a linked state's parameter from `parameter_overrides` uses the native
override-cancellation operation: it restores the linked subtree's default value
and removes bindings targeting that parameter. An explicit binding edit that would
be discarded by the same cancellation is rejected with
`state_tree_binding_rejected_by_native_authoring` before target mutation.
The linked Float case is verified through ordinary workspace import and independent
save/reload readback. A late rejection after save/reload restores the original dirty
override, value, bindings and complete document, independently of the original disk.
Complete state-property reconstruction uses the shared property apply without Details
editing notifications; restoring a state type must not clear parameters already
replayed from that document. Forward cancellation remains separate from exact replay.
Changing a subtree's Float default propagates to its unoverridden linked parameters;
existing override values and bindings remain. The same import can change the subtree
default and an overridden linked value. Forward application uses native propagation
before common expected-state capture. It retains the semantic change draft's actual
field edits and compares their decoded native values after propagation, rejecting
an explicit value that would otherwise be discarded with
`state_tree_parameter_edit_rejected_by_native_authoring`. A linked parameter schema
is fixed by its source; independent schema edits return
`state_tree_linked_parameter_schema_fixed`. Both rejection cases preserve the original
dirty document and disk in the ordinary workspace regression.
Removing the source's last Float field through ordinary HJSON also removes the linked
field, obsolete override and invalid target binding after save/reload. Forward apply
uses native parameter migration and binding cleanup; an empty native bag additionally
uses `ResetParametersAndOverrides` because UE 5.6 leaves stale override IDs in that
case. Exact replay and checkpoint recovery do not perform these editing effects.
A rejection observed after the schema deletion was saved and reloaded restores the
complete original dirty document, native field/override identity and bindings, while
independently restoring the original disk bytes. Partial Float schema deletion is
also verified: the surviving field keeps its native identity, inherited/overridden
value and original binding source; only the removed field's relationships leave.
StateTree update previews and binding validation initialize the duplicated states
from their source object paths before exact document replay, retaining their native
parameter bags despite native duplication changing state IDs. This initialization
does not alter the identity contract of a published native Clone.
Changing a source Float field to Double is verified against native numeric migration
and ordinary HJSON save/reload: field identity remains, unoverridden values take the
new source default, and existing overridden values and compatible bindings survive.
LinkedAsset Float override editing is verified through ordinary HJSON and independent
target save/reload. The external source is compiled and saved first, since linked
defaults come from its compiled parameters. The target retains the external reference,
field identity and binding, while the source's full authoring document, compiled
default and disk bytes remain unchanged. Ordinary override cancellation restores
the external compiled default and removes its target binding. A late rejection
observed after save/reload restores the complete dirty target, external reference,
field/override identity and original target disk independently, while leaving the
source unchanged. Cross-asset source editing, nonnumeric type changes and other
parameter types remain outside this coverage.
GraphCompound state/task creation, naming and child ordering also preserve an
untouched linked Float parameter's native identity, override and binding through
the shared save/reload transaction. ContentGraphRead independently exposes that
same parameter as a named canonical item and reads its retained value through the
shared property codec. This preservation check does not add parameter-editing
operations to GraphCompound's input contract.
Schema property reconstruction retains native editing notifications, which update
derived context descriptors such as the bindable Actor type. Ordinary
`ContextActorClass` editing from Pawn to Character is verified through save/reload.
A late rejection after the new Character state is saved and reloaded restores the
complete dirty Pawn document and derived context, independently restoring the
original Character disk. State-property notifications remain disabled to preserve
exact linked-parameter reconstruction.
The same ordinary import can change the Actor context from Pawn to Character and
create a binding from Character's `CrouchedEyeHeight` to an existing Delay task's
`Duration`. Native path resolution rejects that field on Pawn; native compilation
and ordinary import both accept it after the schema change. Independent reload
retains the new context type and binding endpoints. This proves authoring and
compilation, not runtime value transfer or special PropertyRef/delegate bindings.
PropertyRef and delegate authoring retain the same persistent binding-path carrier;
their special compatibility and compiled-instance initialization remain owned by
native StateTree compilation, which the binding preflight invokes. They have no
separate UAI binding document. Dedicated PropertyRef/delegate cases and runtime
function/value execution are outside the direct evidence above; the representative
Blueprint/Schema coverage does not claim exhaustive type combinations.

The public StateTree asset creation call takes `asset_name` and `package_path`;
populate or edit the created asset through its ordinary HJSON workspace. The
missing-target import branch rejects combining asset creation and content operations
in one request. Ordinary StateTree workspace writes and GraphCompound edits use the
shared Reflection Update transaction. Internal Reflection Create/Clone support is
not evidence of a public full-document StateTree creation or cloning workflow.

## Lifecycle and readback

BehaviorTree readback uses the domain native graph capture for nodes, recursive
subnodes, connections, placement and instance pins. The ordinary workspace export
now consumes graph/node/edge state and shared editable-property requests through
the common composition. It emits readable node/connection aliases, an interface
companion and categorized payload properties without exposing native node GUIDs.
Export does not create a graph for a newly created empty tree or save dirty state.
BehaviorTree semantic-directory import applies graph structure and payload
property changes through the shared Reflection authoring transaction, including
checkpoint, save/reload and final readback. After native graph structure creation,
import binds the Blackboard to both the BehaviorTree asset and its editor Root,
before applying node properties. The same binding owner handles graph mutations,
so Root placement defaults and later editor notifications cannot replace an
authored binding. Shared object authoring recognizes `FBlackboardKeySelector` values,
resolves their native asset context and applies selector fields before dependent
operator/value fields. Native type/ID notifications update runtime derived state;
authors do not supply `SelectedKeyType`, `SelectedKeyID` or `OperationType`.
This applies by reflected value type, including multiple selectors and owned
decorator graph nodes, rather than by a particular decorator class or field name.
EQS selectors that obtain their Blackboard at runtime retain that runtime owner.
The verified creation path uses an
intrinsic root row with `role: root`, native Selector/Wait candidate names and
readable connections; subsequent property and placement edits retain node identity
and compiled execution relationships. If final readback rejects after the Wait node has been
deleted and persisted, the lifecycle restores the complete dirty graph and the different
original disk bytes. A stale `select` rejects before mutation, while a valid native-class selection
reaches creation and persisted readback. `candidate_selector_invalid` leaves the graph unchanged,
and the queried readable name supplies the correction. Same-name BTTask Blueprints return readable
source choices; ambiguous ordinary import leaves the target unchanged, and both ordinary and
incremental imports consume the selected choice. Reloaded task classes match the selected Blueprint.
Existing decorator attachments appear in the same graph `nodes` collection, with
a readable `parent` node alias, `attachment_kind`, and zero-based typed `order`.
Removing a row deletes that attachment; changing `order` reorders the native
Decorator execution list. Generic SubNodes storage retains its independent order.
The attachment workspace test verifies deletion and reorder through ordinary
import, original-object release, clean reload, and compiled decorator order. It
also rejects a persisted attachment deletion at final readback and verifies the
complete dirty memory checkpoint and the different original disk bytes. Native
ownership capture uses persisted collections, not transient ParentNode links.
BehaviorTree attachments and EQS tests share persisted AIGraph ownership capture.
Single-node readback and full graph export carry the same parent reference;
top-level nodes omit `parent` rather than emitting an empty reference.
The ServiceContext test verifies a discovered Default Focus service created with a
new parent, original-object release and clean reload of native/compiled ownership.
Public SDK verification also covers adding it to an existing parent, rejecting an
invalid creation context and editing its Interval through the common property workspace.
For an existing attachment, attachment_kind describes its captured type and cannot
be changed in place; the public SDK rejection test verifies the graph remains unchanged.
Public SDK creation also covers a native Cooldown decorator on the same parent while
preserving its existing service.
Existing CompositeDecorator BoundGraphs export as separate graph files. Their
`owner` uses the common readable item reference, while node and pin aliases remain
local to each graph. Internal test properties use the common property files.
OwnedGraphWorkspace verifies ordinary And-to-Or and position edits, a Cooldown
property edit, original-object release, independent clean reload, and unchanged
main-graph state. It also rejects an internal test-node deletion after persistence
and verifies the complete dirty graph checkpoint and separate original disk bytes.
The same test also creates the outer CompositeDecorator with its new parent,
creates internal NOT and Cooldown nodes from queried names, and expands an OR
node from two inputs to three while connecting a newly created test to the new
input in the same import. AND/OR expose `pin_groups: { inputs: N }` with a minimum
of two inputs; the interface companion describes the current native group.
Result and NOT have fixed inputs. Keep the exported `interface_ref` when editing
the graph, and use the actual exported pin aliases for existing nodes.
The public SDK chain verifies outer creation, readable owner discovery, AND with
three Cooldown tests and connections, input shrink to two, and invalid input-count
rejection with the entire main and owned graphs unchanged. Removing the outer
CompositeDecorator row removes its owned graph while preserving the remaining
main graph. A separate public SDK check reparents an existing CompositeDecorator
from Wait to Selector. After import and independent export, its And with three
Cooldown nodes, connections, and all six graph property files remain unchanged,
including nondefault cooldown values 7.25, 9.5 and 12.75.
RunBehavior's native injected decorator previews retain their source Guid. The
capture scopes each preview to its parent task while keeping the original Guid
inside the checkpoint. Public rows have distinct readable aliases and
`read_only: true`; injected payloads do not expose writable property files.
Ordinary import rejects independent edits to these derived rows. InjectedWorkspace
verifies two tasks referencing one saved subtree, read-only edit rejection with
the full native graph unchanged, and a normal Wait position edit followed by
unload/reload with both previews preserved. It also creates a RunBehavior from
its native candidate, clears one existing source binding through the property
workspace, and verifies that only the corresponding preview disappears after
reload. Native graph initialization hydrates subtree caches inside the transaction;
the native refresh runs on both the isolated proposed tree and the live tree
before execution data is rebuilt. Candidate carrier types come from the native
action template, including RunBehavior's SubtreeTask.
A late-failure case deletes a bound parent and its preview, observes the clean
persisted deletion, then rejects final readback. The complete dirty graph and
the different original disk bytes are restored independently. Native Root
deletion restrictions remain intact.
InjectedCompositeWorkspace covers the same lifecycle with two native copies of
a CompositeDecorator containing an And and two Cooldown nodes. Each BoundGraph
exports separately with a readable owner; the graph and its inner nodes are
read-only. Derived payloads have no writable property files, variable input
interfaces disable adding/removing pins, and candidate creation in a read-only
graph is rejected. Independent inner-graph edits are rejected before mutation.
Capture preserves the native ownership distinction: ordinary BoundGraphs have
one SubGraphs registration, while injected copies have none and must be read-only
with the correct owning node. Normal parent edits, clearing one source reference,
and late-failure recovery of the remaining parent and inner graph are verified;
the editable OwnedGraphWorkspace regression remains passing.
The native-domain incremental graph entry now adapts its prepared
edits to the same domain graph document and Reflection transaction. Its result
oracle runs in the shared final-readback callback; its former independent
BehaviorTree save/rollback path is no longer used. The incremental lifecycle test
checks that the original asset is released before the persisted graph is read.

Success requires the native owner to finish mutation, native editor settle,
compile or domain validation, save, unload/reload, and independent `ContentGraphRead`
capture. Completion follows the native operation's result, not runtime readiness:
Behavior Tree and EQS `UpdateAsset` may produce empty runtime roots/options while
preserving a valid editable graph, and StateTree uses the native compiler's success
result. Actual compiler and structural errors still fail. Disconnected editor nodes
can be imported and saved first; query their instance `pin_interface` aliases before
adding connections in a later import. Successful editing does not establish that
the asset is ready to execute. The
public success result is a compact terminal-status projection; target identity,
revision, persistence receipt, and readback evidence
remain server-owned lifecycle facts. Unsupported roots or operations, stale
revisions, mismatched targets, invalid candidates, and lifecycle failures fail
without a legacy fallback. `content_graph.export_workspace` publishes the
registered Behavior Tree, State Tree, EQS, Blackboard or SmartObject reflection-authoring workspace.
EQS export reads the actual editor graph, including option generators, persisted
test children, enabled/disabled state, order, layout and connections. Generator
and test properties use the common category/codec projection with the native
Details view's ShowDefaultsOnly context. Export does not rebuild the graph from
the runtime Options/Tests arrays, which exclude disabled tests, or dirty the asset.
EQS semantic-directory import projects the editor graph and property changes into
the complete EQS GraphDocument and the common Reflection apply/save/reload path.
Existing-test enable and property edits have persisted through independent reload.
Candidate discovery resolves exported option references with `attachment_kind: test`.
Returned generator/test names can be used unchanged for HJSON creation, including
a new generator and its test in one import. ActorsOfClass/Distance creation and a
new root connection have persisted through independent reload. The specialized
GraphCompound option/layout consumer also projects into the same complete graph
state and Reflection lifecycle; its former manual creation and separate apply
branch have been removed. A nonempty-graph creation test preserves existing tests
and properties through reload. Late rejection after deleting an option and its
tests restores the complete dirty graph and the original disk bytes independently.
For an empty asset, the first generator import includes the native schema's default
root in the planned graph state; export remains read-only. The public SDK chain
has created a generator/test, connected the root and retained edited properties
and a disabled test in re-export. Independent queries now consume the same shared
readable graph projection: `items` returns graph nodes without generic UObject
fragments, `relations` returns readable pin connections and test attachments, and
node names select items and properties. Pin interfaces are available on demand.

Behavior Tree and EQS queries share the canonical authoring graph projection and
the same item/relation assembly. Behavior Tree queries include both the main graph
and CompositeDecorator owned graphs, retain their declared owners and Blackboard
binding, and do not mix in generic internal UObject rows. Node aliases, connection
endpoints and `pin_interface` come from the same projection as workspace export.
Owned graph containers expose the workspace's readable `owner` reference; when the
native graph name is empty, that same reference is their selectable `name`.
Select the intended nodes before requesting their pin interfaces; binding items
and other item kinds do not acquire graph-node fields.
The SDK roundtrip and first query after Editor restart preserve the disabled test
and edited properties. These observations cover the ActorsOfClass/Distance slice.
StateTree structure, scoped properties and parameter bindings use the shared
workspace and Reflection transaction described in [StateTree workspace](#statetree-workspace).
