# T46 Spatial-content operation authority

T46 owns six current public commands. Discover their current hashes and exact
parameter schemas through `client.query.command_search(...)` and
`client.query.command_detail(...)`, then invoke the default generated SDK methods:

```python
client.mesh.element_select_by_ray(...)
client.mesh.pivot_update(...)
client.mesh.transform_bake(...)
client.reference.origin_resolve(...)
client.world.object_align(...)
client.world.object_snap_to_surface(...)
```

The same identities are available through `uai-cli ... exec <command-id>`.
There is no task-specific generated module, compatibility alias, direct HTTP
route, or second spatial/origin implementation in the SDK.

## World transform ownership

`world.object_transform.read` and `world.object_transform.update` are dormant
public identities. Persisted Actor and Component transforms belong to the World
HJSON owner:

- read a World with `content_graph.structure` or select exact Actor/Component
  paths with `content_graph.items`;
- change the selected transform in a `uai.world.edit/1` document and apply it
  with `content_graph.import_workspace` and the exported revision;
- obtain the exact Modeling carrier revision for a StaticMesh from its
  content-graph read, or for an `ADynamicMeshActor` from the selected World HJSON
  actor item's `carrier_revision`.

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

Both commands accept optional `expected_revisions`. When the caller supplies
them, the operation rejects stale state before mutation. When omitted, the
operation freezes the current subject revisions at invocation and still performs
its independent result verification. Invalid frames, missing geometry,
ambiguous vertices, a failed trace, or conflicting fields fail before mutation.
Use World HJSON for an additional persisted-state readback when required.

## Mesh-element selection and durable mesh operations

`mesh.element.select_by_ray` consumes the live selection state owned by Modeling
Tools Editor Mode. Activate the mode through
`mesh.modeling_session(action="activate")`, establish the exact unlocked
component target, and pass that component identity. The command neither activates
the mode nor invents a second selection registry.

`mesh.transform.bake` and `mesh.pivot.update` remain specialized durable geometry
operations. A StaticMesh content-graph read supplies their carrier revision;
success includes package save and remains observable after reload.

## Origin resolution

`reference.origin.resolve` is the sole current origin-query command. Its
`target.source` is one of `catalog_candidate`, `json_reference`, `ue_object`,
`graph_node`, or `name_reference`. It returns canonical asset, package, file and
code origins and is read-only.

## Spatial and lifecycle rules

Spatial values retain their semantic frame, dimension, unit, basis, observation
time and conversion evidence through `frame_id`, `basis_frame_id`, `dimension`,
`unit`, `basis_evidence_id`, and `observed_at`. The SDK forwards one
manifest-bound request and performs no coordinate, selection, trace, pivot,
transform or origin business logic. Tests restore only owned transient state and
preserve the user's Editor.

The historical IDs `level_align_actor_by_bounds`,
`level_align_actor_vertex_to_vertex`, `level_align_face_to_face`,
`level_snap_to_surface`, and `node_origin_resolve` are not public commands or
aliases. Their exact successor contracts are recorded in
`successors/t46_spatial_content_operation.json`.
