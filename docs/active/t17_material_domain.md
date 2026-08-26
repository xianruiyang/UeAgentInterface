# T17 Material HJSON domain

`material-domain-provider` is the sole `content_graph.*` domain provider for
`/Script/Engine.Material`, `/Script/Engine.MaterialFunction`, and
`/Script/Engine.MaterialInstanceConstant`. The import schema is
`uai.material.edit/1`.

`material-domain-owner` also owns the only active Material expression
candidate public command, `material.expression.candidates`. It queries the
real Material Graph action schema for one persisted Material or Material
Function. The exact creation context is carried separately as `graph_name` and,
for pin-drag menus, `node_guid`, `pin_guid`, and `pin_name`. Results are bounded
and explicitly paged with `complete`, `has_more`, and `next_cursor`. Each native
action has a compact `id`, display `name`/`label`, action kind, support state,
and operation support. Distinct schema actions such as Make Float2, Make Float3,
and Make Float4 retain distinct identities even when they share an expression
class. The inactive legacy `reference.catalog_search` binding is neither an
ingress nor a fallback for this domain.

The public SDK calls are `client.material.expression_candidates(...)` and
`client.content_graph.import_workspace(...)`. Use
`client.query.command_search(...)` and `client.query.command_detail(...)` to
discover and inspect their live contracts; `uai-cli` exposes the same discovery
path. No task-specific helper or tutorial-owned registry is authoritative.

Material and Material Function documents use explicit `edits`; Material
Instance documents use explicit `parameters`. The provider maps those fields to
the already-authoritative T50 Material mechanics and does not copy expression,
parameter, compile, persistence, or recursive initialization behavior.
Material expressions resolve `uai.init.material.expression/1` through the
common T13 provider registry seam. T13 is the sole recursive initialization
and source-carrier authority; T17 consumes T13's normalized
`source.kind=resolved` document set and contributes only the Material domain
contracts. Unsupported root types and create/edit combinations without an
atomic native contract fail before mutation.

Every expression-create edit carries only the selected action and its separate
native creation context:

```hjson
value: {
  candidate: { id: "candidate:..." }
  candidate_context: {
    graph_name: MaterialGraph
    node_guid: "<optional-source-node-guid>"
    pin_guid: "<optional-source-pin-guid>"
    pin_name: "<optional-source-pin-name>"
  }
}
```

`candidate` minimally contains `name` or `id`; if both are supplied they are
conjunctive. A known expression class may be supplied as an assertion, but it
does not replace the native action identity. Graph/pin context, stable element
identity, layout, connections, properties, and operation stay outside the
candidate. Before the first write the owner rebuilds the same Graph/Pin action
schema, resolves exactly one action, and uses its official expression factory,
delegates, and autowire behavior. Missing, ambiguous, context-mismatched, or
unsupported actions fail before mutation; callers do not copy a complete search
row or binding revision into HJSON.

Queries capture the actual Material owner through the common content-graph
read environment and return its stable revision, items, relations, parameters,
diagnostics, and compile state. Imports require the observed revision, invoke
the exact native Material command, then independently recapture the persisted
owner. Creation is registered for Material and Material Instance roots;
Material Function creation remains explicitly unregistered.

T17 does not register a Material workspace-export serializer. The shared
`content_graph.export_workspace` command therefore fails explicitly with
`material_domain_export_not_registered` for these roots instead of returning a
query snapshot or publishing a workspace that cannot round-trip through the
Material import schema.
