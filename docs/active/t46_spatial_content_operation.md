# T46 Spatial-content operation authority

T46 owns five current public commands. Discover command names with
`client.query.command_search(...)`, then obtain selected exact IDs' SDK examples
with `client.query.command_usage([...])`. Use `ueagentinterfacecmd describe <command-id>`
for public parameter shapes before invoking the selected method when those fields are unknown:

```python
client.mesh.element_select_by_ray(...)
client.mesh.transform_bake(...)
client.reference.origin_resolve(...)
client.world.object_align(...)
client.world.object_snap_to_surface(...)
```

The same identities are available through `ueagentinterfacecmd ... exec <command-id>`.
There is no task-specific generated module, compatibility alias, direct HTTP
route, or second spatial/origin implementation in the SDK.

## World transform ownership

`world.object_transform.read` and `world.object_transform.update` are dormant
public identities. Persisted Actor and Component transforms belong to the World
HJSON owner:

- read a World with `content_graph.structure` or select exact Actor/Component
  paths with `content_graph.items`;
- export the Level through `client.content_graph.export_workspace(...)`, edit the
  selected transform's declared HJSON fields and apply with `workspace.import_changes()`;
- use the readable StaticMesh or `ADynamicMeshActor` target for specialized
  mesh operations. Their carrier revision remains private to the server owner.

This retirement covers persisted authoring state. It does not define a PIE or
runtime-state inspection protocol; that is a separate product surface.

Normal command search and generated named SDK methods hide both dormant
identities. Development-only compatibility search uses
`query.command.search` with `mode="compatibility"` and returns their lifecycle,
active successors and user-confirmation requirement. If the active HJSON path
cannot satisfy an authorized task, explain the exact gap and obtain explicit
user agreement before using the guarded dormant SDK/CLI entry point. The
original command contracts and C++ handlers remain executable; no per-command
compatibility implementation is introduced.

## Actor alignment and surface snap

`world.object_align` retains four native alignment mechanics selected by
`alignment.kind`: explicit anchors, bounds anchors, signed faces, or StaticMesh
world vertices. `world.object_snap_to_surface` retains bounded Editor-world ray
placement. These specialized operations are not generic transform CRUD.

Both commands resolve every subject on the server and freeze their current
state during preflight; reference-based alignment also freezes the reference
object. The owner rechecks all of them before any write, so one changed object
cannot cause a partial alignment. Surface snap repeats its bounded trace at the
write boundary. Callers never submit revisions. Invalid frames, missing
geometry, ambiguous vertices, a failed trace, or conflicting fields fail before
mutation. Use World HJSON for an additional persisted-state readback when
required.

## Mesh-element selection and durable mesh operations

`mesh.element.select_by_ray` consumes the live selection state owned by Modeling
Tools Editor Mode. Activate the mode through
`mesh.modeling_session(action="activate")`, establish the exact unlocked
component target, and pass that component identity. The command neither activates
the mode nor invents a second selection registry.

`mesh.transform.bake` remains a specialized durable geometry operation. Persisted
pivot desired state uses HJSON; `mesh.pivot.update` is dormant. Transform bake
resolves the readable target and keeps the carrier revision in the same server
operation; success includes package save and remains observable after reload.

## Origin resolution

`reference.origin.resolve` is the sole current origin-query command. Its
`target.source` is one of `catalog_candidate`, `json_reference`, `ue_object`,
`graph_node`, or `name_reference`. It returns canonical asset, package, file and
code origins and is read-only.

## Spatial and lifecycle rules

Native failures confirmed before any target write return `mutation_started=false`
and `retry_safe=true`, preserving the original diagnostic. Once a native write may
have occurred, later failures retain conservative mutation and recovery status;
an earlier successful item or patch field must not be forgotten. The native
families report these facts through the shared execution receipt; error names do
not determine whether a write occurred.

The minimal public spatial value uses `quantity`, `frame_id`, `dimension`, `unit`
and `components`; frame-only inputs use `frame_id`, `dimension` and `unit`.
The accepted rich representation preserves its nested frame, subject and conversion
evidence. Optional `basis_frame_id`, `basis_evidence_id` and `observed_at` describe
that richer observation; callers do not invent them to satisfy a compact input.
The SDK forwards one
manifest-bound request and performs no coordinate, selection, trace, pivot,
transform or origin business logic. Tests restore only owned transient state and
preserve the user's Editor.
