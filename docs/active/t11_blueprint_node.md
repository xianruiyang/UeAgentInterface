# T11 Blueprint node authority

The only public SDK binding is
`client.blueprint.node_candidates(...)`; `uai-cli ... exec
blueprint.node.candidates` exposes the same exact command. Callers use
`command_search` and `command_detail` to obtain its current fields and hash.
There is no task-specific generated projection or independent SDK authority.

`blueprint-node-authoring-owner` owns the exact read-only
`blueprint.node.candidates` public command. It resolves one existing Blueprint
and one stable graph identity against Unreal's real `FBlueprintActionDatabase`;
the returned `candidate.name`, `binding_revision`, `graph.id`, and
`graph_revision` are opaque exact identities and are copied unchanged into the
HJSON authoring request.

Blueprint HJSON uses `uai.blueprint.node.edit/1`. The process-frozen common
domain registry resolves the Blueprint root to
`uai.init.blueprint.graph/1` and each graph-owned Branch entry to
`uai.init.blueprint.node/1`. The provider performs complete preflight before
write, invokes the resolved `UBlueprintNodeSpawner`, creates normal Pins and
connections, compiles, saves, unloads, reloads, and returns an independent
readback. Stale revisions, changed candidate bindings, unsupported node types,
wrong graphs, incompatible connections, compile failures, save failures, and
reload failures are terminal typed failures; they do not fall back to a legacy
node owner.

The same provider serves the shared `content_graph.*` query, import, and export
contracts for Blueprint targets. Export uses the common verified staging and
atomic publisher. Query commands are read-only and report complete bounded
results. This provider does not own or duplicate the shared public
`content_graph.*` command declarations.
