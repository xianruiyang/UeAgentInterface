# T11 Blueprint node candidate authority

`blueprint.node.candidates` is the sole public candidate query for the current
Blueprint node creation surface. It resolves one existing Blueprint and one
exact graph/pin context against Unreal's native Blueprint Action Menu source.
The public call is `client.blueprint.node_candidates(...)`. Use
`client.query.command_search(...)` for discovery and
`client.query.command_usage(...)` for the minimal Python form; generated SDK/formal schema is
only for an exceptional schema or failure question.

Results contain names plus one query `scope`; pin-context queries also return one
`candidate_context` object using the same node/pin aliases accepted by HJSON:

```json
{"names":["If Then Else"],"scope":{"target":"/Game/Blueprints/BP_Example","graph_name":"EventGraph"}}
```

Repeated representations of the same canonical native action are merged. Same-name
semantic differences share one display name while retaining their native identities.
Equal public selector fields do not merge distinct native identities; insufficient
semantic distinctions produce a diagnostic instead of choosing the first action.
Resolution accepts `name` plus at most two returned short `select` fields.
Class-based choices use the short native class name, while native rebinding
retains the complete class identity internally. Ordinary graph rows carry
`name` and `select` together; wrapping the row as a candidate preserves both.
An actual ambiguity returns its reason and replayable choices. Each choice uses
its own one or two semantic fields. Choices using different fields are returned
as an array of objects; copy one complete object to `select`. If the native
catalog cannot provide sufficient public semantic fields, resolution reports
`candidate_discriminator_missing` rather than inventing a scoped name.

Every node candidate adds specialized function, event, variable, macro or operator
fields to its node class and asset/class bindings; specialization never clears
those independent facts. Ordinary functions retain their function owner and name.
Promotable operators use `operation_name`: their registered representative function
is not a semantic choice, because pin promotion determines the concrete function.
A function's owner and name do not identify its node form: ordinary interface calls and interface
messages may refer to the same function. These ambiguities offer the readable
`select.node_class` (and class source when needed), then replay the selected
native spawner. The internal action signature is not a public selector.

Native input variants retain their binding as semantic selection fields. A unique
reflected `FKey` contributes `input_key` using its stable key name; this also covers
debug-key nodes and inherited scalar/vector axis event and value nodes without
depending on their localized menu labels or private plugin classes. Legacy mapped
Action and Axis nodes contribute `input_action` and `input_axis` from their native
mapping names. For example, an `Input Key` ambiguity can offer `input_key: "W"`.
Copy the returned choice; a search for `W` does not bind the subsequent creation.
Enhanced Input asset nodes retain the existing `asset_override` binding mechanism.
The same native input-binding capture is used for actual-node observation:
`content_graph.items` exposes an `input_binding` object containing these
semantic fields even when the native property is not editable. This is query
metadata, separate from the writable workspace node spec and from Enhanced Input's
existing asset-path `input_action` field; it does not add a second creation path.

Reuse the names within this target, graph and optional pin context. Switching
asset, graph or pin context requires discovering the needed names there; an
Actor Blueprint result does not establish availability in an Animation or Widget
Blueprint. A changed member/component catalog can require refreshing affected
names in the same graph. `scope` is response metadata and is not copied into HJSON.

Component-bound events retain their component binding in catalog identity before
deduplication. Components sharing a delegate retain separate identities behind
the name `On Component Hit`; ambiguity offers `select.component_name`, for
example `OrbCollision`. Component names remain searchable. Read back `component_name` and delegate identity
after creation to verify the selected binding.

Resolve the graph context before querying nodes. Call
`content_graph.structure` for the Blueprint, select the intended graph in
`payload.entries` by `graph_kind` (for example `event_graph`), then pass its `name` to
`blueprint.node.candidates.graph_name`. Do not cache or reconstruct a graph GUID.
Named graph containers preserve the authored name used by HJSON; node occurrence
aliases must not be applied to graph names. This is shared by Actor, Widget and
Animation Blueprints.

Background creation retains the selected native action's bindings, node hooks,
autowiring and modification notifications. It does not invoke menu-only editor
opening, selection or Goto behavior, including for ghost replacement and reuse.
Native view/navigation actions are search-only rather than creation candidates.
Explicit editor opening and preview actions remain separate operations.

The same public structure is also the compact readback for existing Blueprint
nodes. Public node entries reuse the Blueprint authoring state owner for semantics:
events expose `event_name`/`event_class`, component-bound events expose
`component_name` and delegate identity, function calls expose
`function_name`/`function_owner_class`, and variable nodes expose
`variable_name`/`access`. Use those fields to select or verify nodes; do not
infer identity from position, node count, edges, or the raw K2 class, and do
not export a full workspace merely to recover information already present in
this compact structure.
For pin-drag candidate context, pass the exported node and pin aliases unchanged.
They resolve through the same authoring alias projections as HJSON, not by
stripping numeric suffixes. Legacy semantic names plus explicit occurrences remain
accepted; without an occurrence the exact public alias takes precedence.

Add one minimal selector to the current exported graph file's `nodes` map:

```hjson
nodes: {
  BranchGate: {
    candidate: { name: "If Then Else" }
  }
}
```

Keep the graph's existing entries. Use the returned candidate name rather than
assuming it equals the search text or menu display label. The node map key is a
local alias; the workspace diff derives creation, update and removal, so ordinary
rows do not contain `operation`. Pin-drag context, when required, is carried separately in
`candidate_context`; it is not part of `candidate`. Pin-context discovery returns
one `candidate_context` object for all returned names. Copy that object unchanged
onto the new node; it already resolves legacy names and occurrences to current aliases:

```hjson
SprintSpeed: {
  candidate: { name: "Set Max Walk Speed" }
  candidate_context: { node: "CharacterMovement", pin: "CharacterMovement" }
}
```

Here `CharacterMovement` is an existing getter node and its exported output Pin
alias. The candidate name alone does not retain a previous query's pin context;
creation re-enumerates that menu. Background-menu discovery omits `candidate_context`.
This contract is shared by ordinary Blueprint, Widget and Animation graphs.
There is no active
`uai.blueprint.node.edit/1` root and no node-specific public apply command.
The process HJSON authority rejects that retired schema; only the
Blueprint authoring owner performs persistent node creation. UMG ordinary
workspaces also delegate existing-graph candidate creation to this same query,
normalizer and native action owner; see T15 for that asset carrier.

Graph dependencies use the readable node and Pin keys from the current exported
document; JSON or HJSON map-key order does not control creation.
Read the refreshed workspace after creation or replacement for the settled
instance interface, including variable Pin counts.

Before persistent mutation, the Blueprint Surfaces provider re-enumerates the
same native Action Menu for the same Blueprint, graph and optional pin context,
resolves the readable `name` and optional short `select` uniquely, and invokes the resolved native
`FEdGraphSchemaAction::PerformAction`. It does not translate a display name to a
node class or reconstruct an action in Python or the HJSON adapter. Unknown,
ambiguous, stale, context-mismatched, or assertion-mismatched candidates fail
before write. Preflight creation resolves again in its validation Blueprint;
the authoring target path in a diagnostic does not imply that its native menu
used the persistent Blueprint object. Native registration requires asset
eligibility on both the validation Blueprint and its private outer package.
Their original transient/public flags are restored immediately after synchronous
action registration, before menu projection or mutation; the global transient
package and asset registry are not changed. Missing member diagnostics distinguish the
current validation class field, registered action, menu entry, and projected name.
They do not substitute another node or relax selection and ambiguity constraints.

One native Action Menu selection may create a primary node plus UE-owned
companion nodes in the same graph. Component-bound function actions are one
example: Unreal may create the call and its bound variable getter together.
This remains one candidate execution; the authoring surface returns the new
primary node and preserves every companion and native connection created by
that same action. It does not require callers to recreate those companions or
encode them as additional candidates. An action is rejected if its returned
primary is not among the nodes newly created in the requested graph.
Companions remain ordinary persisted graph nodes and are visible in structure
readback; there is no hidden companion registry or second ownership state.
Later edits address those read-back identities or replace the graph cohesively,
matching the graph state Unreal actually created.

Successful Blueprint-root import compiles, saves, unloads, reloads and reads
back the persisted graph through the shared content-graph lifecycle. Candidate
discovery itself is read-only and does not replace the public content-graph
revision required by `content_graph.import_workspace`.

The surface validates the readable selector against the real target and repeats
the same resolution in duplicated preflight. Missing, stale or ambiguous
rebinding is a typed failure that identifies the public graph node key without
exposing the internal spawner signature.
Missing/ambiguous action diagnostics retain the requested name, authoring target,
current graph/menu context and corrective query guidance, along with the ordinary
HJSON source location. They do not infer which previous query supplied the name.
