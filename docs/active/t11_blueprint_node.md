# T11 Blueprint node candidate authority

`blueprint.node.candidates` is the sole public candidate query for the current
Blueprint node authoring surface. It resolves one existing Blueprint and one
exact graph identity against Unreal's `FBlueprintActionDatabase`. The response
contains the public graph object, a read-only `graph_revision`, and a bounded
`candidates` array. Each candidate currently has this executable shape:

The public call is `client.blueprint.node_candidates(...)`. When its method or
hash is not already known, use `client.command_search(...)` followed by
`client.command_detail(...)`; `uai-cli` exposes the same exact contract.

```json
{
  "name": "/Script/BlueprintGraph.K2Node_IfThenElse",
  "binding_revision": "sha256:...",
  "display_name": "Branch",
  "graph_id": "graph:..."
}
```

The candidate object is an opaque, server-issued binding and is copied
unchanged to
`provider_extension.graphs.<stable-graph-key>.nodes.<stable-node-id>.candidate`
inside the Blueprint-root `uai.blueprint.edit/1` document. There is no active
`uai.blueprint.node.edit/1` root and no node-specific public apply command. The
graph map key remains the Blueprint HJSON stable identity (for example
`event_graph:EventGraph`); it is not replaced by `candidate.graph_id`.

Before any persistent write, the Blueprint Surfaces provider re-runs the same
candidate mechanics for the exact target, graph, executable name, and binding
revision. It verifies that `candidate.graph_id` identifies the owning graph and
then expands the internal mutation fields `node_type="node_by_class"` and
`node_class=candidate.name`. Callers neither reconstruct nor duplicate those
fields. If callers provide them, they must match the candidate exactly.

Changed graph context, executable identity, or binding revision fails with a
typed candidate-binding error before mutation; a stale binding specifically
returns `blueprint_surface_node_candidate_binding_stale`. Candidate discovery
does not select a result, mutate Unreal state, or substitute the public
content-graph revision required by `content_graph.import_workspace`.
