# T11 Blueprint node candidate authority

`blueprint.node.candidates` is the sole public candidate query for the current
Blueprint node creation surface. It resolves one existing Blueprint and one
exact graph/pin context against Unreal's native Blueprint Action Menu source.
The public call is `client.blueprint.node_candidates(...)`. When its method or
contract hash is not already known, use `client.query.command_search(...)` followed
by `client.query.command_detail(...)`; `uai-cli` exposes the same exact contract.

Candidate rows expose compact creation selectors:

```json
{
  "name": "Branch",
  "id": "candidate:...",
  "display_name": "Branch",
  "kind": "blueprint_node"
}
```

`name` is sufficient only when it is unique in the exact native context. `id`
is the precise selector and preserves the native action identity. Other
returned fields are diagnostics or optional assertions; callers do not need to
copy a complete search row. Continue with `next_cursor` while `has_more=true`
before making an exhaustive or unique-result claim; one page is bounded to 20
rows.

Specialized rows also expose the safe semantics needed to select without a
localized display-name inference:

- function calls: `function_name`, `function_owner_class`;
- events: `event_name`, `event_class`;
- variables: `variable_name`, `node_type=get|set`, `member_parent_class`;
- generic nodes: `node_class`;
- Unreal promotable operators: `node_class`, `operation_name`.

Promotable operators such as `Multiply` and `EqualEqual` are native shared
Action Menu nodes whose concrete function is selected from connected pin types. They
are not fixed function-call candidates. Their row therefore keeps the native UI
name and compact `id`, exposes `operation_name` as the optional stable semantic
assertion, and does not expose the spawner's registration-order representative
`function_name` or `function_owner_class`.

`operation_name` preserves Unreal's native literal semantic name; callers must
not derive or normalize it from the localized menu label. For example, an
object equality row may display `Equal ( == )` while its exact assertion is
`operation_name: EqualEqual`.

`native_action_signature` and internal spawner identity are deliberately not
public. A caller that already knows more may copy any returned semantic field
beside `name` or `id` as an exact assertion. The assertion never replaces the
selector; a missing, non-string or changed value fails before write with
`blueprint_surface_node_candidate_metadata_mismatch`.

Resolve the graph context before querying nodes. Call
`content_graph.structure` for the Blueprint, select the unique container whose
`kind` is `blueprint_graph`, `value.name` is `EventGraph`, and
`value.graph_kind` is `event_graph`, then copy that container's `id` unchanged
into `blueprint.node.candidates.graph_id`. The public structure row deliberately
keeps the opaque graph ID next to these selectable semantics; callers must not
infer a graph from its nodes, cache an earlier GUID, or reconstruct the ID.

The same public structure is also the compact readback for existing Blueprint
nodes. Each node row keeps its stable `id` and `class_path`, while `value`
reuses the Blueprint authoring state owner for directly selectable semantics:
events expose `event_name`/`event_class`, component-bound events expose
`component_name` and delegate identity, function calls expose
`function_name`/`function_owner_class`, and variable nodes expose
`variable_name`/`access`. Use those fields to select or verify nodes; do not
infer identity from position, node count, edges, or the raw K2 class, and do
not export a full workspace merely to recover information already present in
this compact structure.

Use only one minimal selector in the Blueprint-root `uai.blueprint.edit/1`
document:

```hjson
graphs: {
  "event_graph:EventGraph": {
    nodes: {
      "node:BranchGate": {
        operation: create
        candidate: {
          id: "candidate:..."
          node_class: "/Script/BlueprintGraph.K2Node_IfThenElse"
        }
      }
    }
  }
}
```

The graph map key remains the graph context and the node map key remains the
stable node identity. Pin-drag context, when required, is carried separately in
`candidate_context`; it is not part of `candidate`. There is no active
`uai.blueprint.node.edit/1` root and no node-specific public apply command.
The process HJSON authority rejects that retired schema; only the
Blueprint-root `uai.blueprint.edit/1` provider owns persistent node creation.

For `operation: replace`, a candidate may use a source node that is rebuilt in
the same graph document. The mutation owner creates nodes in
`candidate_context.node_guid` dependency order before applying edges; JSON or
HJSON map-key order does not control creation. Refer to a rebuilt source with
`node_guid` plus `pin_name`. A persisted `pin_guid` is suitable only while that
exact source pin survives, because rebuilding a node does not preserve its old
pin GUID.

Before persistent mutation, the Blueprint Surfaces provider re-enumerates the
same native Action Menu for the same Blueprint, graph and optional pin context,
resolves `name` or `id` uniquely, and invokes the resolved native
`FEdGraphSchemaAction::PerformAction`. It does not translate a display name to a
node class or reconstruct an action in Python or the HJSON adapter. Unknown,
ambiguous, stale, context-mismatched, or assertion-mismatched candidates fail
before write.

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

The surface first validates the public `id` exactly against the real target.
Its mutation preflight then runs on a duplicated validation Blueprint, where
asset-local spawner identities can acquire a different owner path. The
normalizer freezes the exact native name plus clone-stable safe semantics from
the already validated row; only the scratch preflight may use that frozen tuple
to rebind uniquely. Direct public candidate execution still rejects an invalid
`id`, and a missing or ambiguous scratch rebind remains a typed failure. That
failure identifies the public graph node key and selector without exposing the
internal spawner signature.
