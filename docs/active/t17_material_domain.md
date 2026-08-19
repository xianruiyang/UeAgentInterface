# T17 Material HJSON domain

`material-domain-provider` is the sole `content_graph.*` domain provider for
`/Script/Engine.Material`, `/Script/Engine.MaterialFunction`, and
`/Script/Engine.MaterialInstanceConstant`. The import schema is
`uai.material.edit/1`.

`material-domain-owner` also owns the only active Material expression
candidate public command, `material.expression.candidates`. It queries the
real Material palette for one persisted Material or Material Function and
returns `name`, `label`, `kind`, `supported_operations`, `input_contract`,
`placement_contract`, `required_context`, and `binding_revision`. Results are
bounded and explicitly paged with `complete`, `has_more`, and `next_cursor`.
`name` is the exact expression class path accepted by HJSON; `label` is display
text and is never executable. The inactive legacy `reference.catalog_search`
binding is neither an ingress nor a fallback for this domain.

The public SDK calls are `client.material.expression_candidates(...)` and
`client.content_graph.import_workspace(...)`. Use
`client.command_search(...)` and `client.command_detail(...)` to discover and
inspect their live contracts; `uai-cli` exposes the same discovery path. No
task-specific helper or tutorial-owned registry is authoritative.

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

Every expression-create edit carries the queried candidate unchanged beside
the native class reference:

```hjson
value: {
  class_ref: "/Script/Engine.MaterialExpressionScalarParameter"
  candidate: {
    name: "/Script/Engine.MaterialExpressionScalarParameter"
    binding_revision: "sha256:..."
  }
  position: { x: -300, y: 0 }
}
```

Before T50 mechanics receives the edit, the Material owner verifies that
`candidate.name == class_ref` and re-resolves the binding against the current
real palette. Missing, renamed, or stale candidates fail before write; only the
validated metadata wrapper is removed before forwarding the unchanged native
edit contract.

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
