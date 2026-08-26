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
copy a complete search row.

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
        candidate: { id: "candidate:..." }
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

Before persistent mutation, the Blueprint Surfaces provider re-enumerates the
same native Action Menu for the same Blueprint, graph and optional pin context,
resolves `name` or `id` uniquely, and invokes the resolved native
`FEdGraphSchemaAction::PerformAction`. It does not translate a display name to a
node class or reconstruct an action in Python or the HJSON adapter. Unknown,
ambiguous, stale, context-mismatched, or assertion-mismatched candidates fail
before write.

Successful Blueprint-root import compiles, saves, unloads, reloads and reads
back the persisted graph through the shared content-graph lifecycle. Candidate
discovery itself is read-only and does not replace the public content-graph
revision required by `content_graph.import_workspace`.
