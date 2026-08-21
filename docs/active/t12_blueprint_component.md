# T12 Blueprint Component candidate authority

`blueprint.component.candidates` is the sole public candidate query for the
current Blueprint Simple Construction Script creation surface. It accepts one
exact Blueprint target, one supported Component class, and one proposed
variable name. The response contains the read-only `scs_revision`, current SCS
readback, and a bounded `candidates` array. An available candidate has this
executable shape:

The public call is `client.blueprint.component_candidates(...)`. When its
method or hash is not already known, use `client.command_search(...)` followed
by `client.command_detail(...)`; `uai-cli` exposes the same exact contract.

```json
{
  "name": "PointLight",
  "component_class": "/Script/Engine.PointLightComponent",
  "binding_revision": "sha256:..."
}
```

Copy the complete candidate object unchanged to
`provider_extension.components.<stable-component-key>.candidate` inside the
Blueprint-root `uai.blueprint.edit/1` document. There is no active
`uai.blueprint.component.edit/1` root and no Component-specific public apply
command. The HJSON entry still owns operation, stable map identity, parent, and
properties; the candidate does not invent those authoring decisions.

Before any persistent write, the Blueprint Surfaces provider re-runs the same
candidate mechanics for the exact target, class, requested name, and binding
revision. It then expands the internal mutation fields
`name=candidate.name` and `class=candidate.component_class`. Callers neither
reconstruct nor duplicate those fields. If callers provide them, they must
match the candidate exactly.

Changed SCS state, name availability, class, or binding revision fails before
mutation; a stale binding specifically returns
`blueprint_surface_component_candidate_binding_stale`. The query-level
`scs_revision` remains read-only evidence and does not replace the public
content-graph revision required by `content_graph.import_workspace`.
Successful Blueprint-root import compiles, saves, unloads, reloads, and reads
back the persisted SCS through the shared T36 lifecycle seam.
T13 remains the sole source-carrier and common recursive initialization
authority; this candidate surface neither reloads documents nor registers a
parallel recursion owner.
