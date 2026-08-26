# T14 Blueprint Surfaces

`client.content_graph.import_workspace(...)` is the only public Blueprint HJSON mutation
ingress. Its `source` union accepts `inline`, `single_file`, and `directory`
carriers; no Blueprint-specific apply command is public. Shared
`content_graph.*` reads and `content_graph.export_workspace` are the public
inspection and export paths.

When the generated method or contract is not already known, use
`client.query.command_search(...)` and `client.query.command_detail(...)`; `uai-cli`
exposes the same discovery path.

The accepted entry is `uai.blueprint.edit/1` with exact root type
`/Script/Engine.Blueprint`. The Blueprint Surfaces provider uniquely owns
ordinary Blueprint settings, parent, interfaces, class defaults, members,
component hierarchy, and managed graphs, nodes, pins, and edges. T13 owns
carrier loading and recursive initialization. T14 consumes only the T36
resolved-document and lifecycle seam; it does not reopen source files, build a
second result envelope, or claim success without an explicit persistence
receipt and independent readback.

## Candidate-bound node and Component creation

Use `blueprint.node.candidates` and `blueprint.component.candidates` when adding
their supported objects. Put only one direct `name` or `id` selector into the
target entry:

```hjson
provider_extension: {
  components: {
    "component:PointLight": {
      operation: create
      candidate: { id: "candidate:..." }
      parent: DefaultSceneRoot
    }
  }
  graphs: {
    "event_graph:EventGraph": {
      operation: update
      graph: { graph_kind: event_graph }
      nodes: {
        "11111111-1111-1111-1111-111111111111": {
          operation: create
          candidate: { name: Branch }
          pos: { x: 800, y: 600 }
        }
      }
    }
  }
}
```

The Component stable-map key owns the instance name; the graph identity and
optional pin context remain outside the node candidate. The provider
re-enumerates the same Add Component registry or Blueprint Action Menu before
the first persistent write and invokes the same native action. Optional richer
fields are assertions only. Unknown, ambiguous, stale, mismatched-context, or
conflicting candidates fail before mutation and leave the public asset revision
unchanged; there is no class/spawner fallback.

Create uses `expected_revision="missing"`; update and upsert copy the latest
public content-graph revision unchanged. Success is terminal only after native
compile/settle, save, package unload/reload, and independent readback of all
intended surfaces. Unsupported roots or creation roles, identity mismatch,
duplicate or missing entry documents, stale revisions, unresolved type
identities, compile or persistence failures are typed terminal failures with
no retired-owner fallback or residue.
