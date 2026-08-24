# T46 Spatial-content operation authority

T46 owns eight current public commands. Discover their current hashes and exact
parameter schemas through `client.command_search(...)` and
`client.command_detail(...)`, then invoke the default generated SDK methods:

```python
client.mesh.element_select_by_ray(...)
client.mesh.pivot_update(...)
client.mesh.transform_bake(...)
client.reference.origin_resolve(...)
client.world.object_align(...)
client.world.object_snap_to_surface(...)
client.world.object_transform_read(...)
client.world.transform_candidates_evaluate(...)
```

The same identities are available through `uai-cli ... exec <command-id>`.
There is no task-specific generated module, compatibility alias, direct HTTP
route, or second spatial/origin implementation in the SDK.

## Actor alignment

`world.object_align` has one public contract. Its required `alignment.kind`
selects exactly one of four native mechanics:

- `anchors`: provide `subject_anchor` and `reference_anchor` as explicit 3D
  points. Their declared frames are converted before the actor delta is applied.
- `bounds`: provide `axis`, `source_anchor`, and `target_anchor`; anchors are
  `min`, `center`, or `max` on the real Actor component bounds.
- `face`: provide signed `source_face` and `target_face`, such as `+x` and
  `-x`, plus optional `offset_cm`. The signed face determines axis and bound.
- `vertex`: optionally provide both vertex indices. If both are omitted, the
  real StaticMesh world vertices must have one unique nearest pair; a tie fails
  before mutation.

Non-anchor modes require the exact `anchor_ref`. Every write supplies
`subject_refs`, current `expected_revisions`, a three-dimensional
`verify_frame`, and explicit spatial evidence. Use
`client.world.object_transform_read(...)` after the operation for independent
transform readback. Invalid frames, stale revisions, missing geometry,
ambiguous nearest vertices, or incomplete mode fields fail before mutation.

## Actor and StaticMesh readback

`world.object_transform.read` is the single revision-bearing read entry for the
WorldSpatial operation family. Its exact target union is an Actor or a
StaticMesh asset:

- an Actor target returns its independently observed transform in the requested
  convertible three-dimensional frame;
- a StaticMesh target requires the identity `mesh.geometry.local`,
  three-dimensional, centimeter frame and returns its real mesh-description
  vertices plus any live component-placement observations.

Both branches return the revision produced by the same WorldSpatial runtime and
session that validates subsequent writes. A client can therefore pass that
revision unchanged to the target's `expected_revision` entry. Other UObject
types and non-local StaticMesh verify frames fail before any mutation. Durable
`mesh.transform.bake` and `mesh.pivot.update` success includes package save;
reloading the asset must preserve the observed geometry.

## Mesh-element selection session

`mesh.element.select_by_ray` consumes the live selection state already owned by
Modeling Tools Editor Mode. Activate that mode explicitly through
`mesh.modeling_session(action="activate")`, establish and unlock the intended
mesh target, then pass the exact `UPrimitiveComponent` object path in
`context.selection_identities`. An Actor path is not a mesh-target identity.
The viewport identity is exact, for example `level:0`; the command does not pick
an implicit current viewport.

Production resolution reuses the active
`UModelingToolsEditorMode::GetSelectionManager()` when no explicit test or
custom binding was supplied. Resolution does not activate the mode, select an
Actor or Component, register or unlock a target, focus a viewport, or create a
second selection registry. A supplied `trace_query` first requires the World
trace to hit the same target; omit it when only the Modeling selection ray is
required. Success is verified by an independent readback from that same live
manager. Invalid or inactive component identities, locked targets, ray misses,
and occlusion fail without changing the existing mesh-element selection.

## Transform-candidate limitation

`world.transform_candidates.evaluate` is registered but is not currently a
production-usable evaluator. Its public descriptor requires per-candidate
`score` results, while the current production reader does not consume the
`candidates` array and no authoritative scoring objective, units, normalization,
or ordering has been defined. Do not treat its current transform observations as
candidate evaluation. The command remains deferred until that result contract is
resolved; the other spatial commands do not depend on it.

## Origin resolution

`reference.origin.resolve` is the sole current origin-query command and belongs
to QueryService, AssetProjectRead's reference provider, and ReferenceQueryOwner.
Its `target.source` is required and is one of:

- `catalog_candidate`: `candidate_id` is an exact ID returned by the current
  catalog search.
- `json_reference`: `json_file` and `json_pointer` resolve an identity object.
- `ue_object`: `full_name` identifies a real loaded or loadable UE object.
- `graph_node`: `full_name`, `graph_name`, and `node_guid` identify a graph node.
- `name_reference`: `kind`, `name`, and `full_name` provide an explicit identity.

The query returns canonical target fields, `origin.asset_paths`,
`origin.package_paths`, `origin.package_files`, `origin.code_paths[]`,
`resolved`, `status`, and `warnings`. It is read-only; unsupported or incomplete
sources return a typed failure without changing UE state.

## Spatial and lifecycle rules

Spatial values retain `semantic_frame_id`, native write/read/verify frame,
dimension, unit, basis evidence, observation time, and conversion evidence.
The SDK forwards one manifest-bound request and performs no coordinate,
selection, trace, pivot, transform, or origin business logic.

Selection is Editor-transient and cleanup restores only the owned selection.
Actor and mesh mutations reach success only after the declared mechanics and
independent readback agree. Tests use owned actors, saved mesh assets, graph nodes,
JSON files, catalog identities, and transient packages, then remove or restore
only those fixtures while preserving the user's Editor.

The historical IDs `level_align_actor_by_bounds`,
`level_align_actor_vertex_to_vertex`, `level_align_face_to_face`,
`level_snap_to_surface`, and `node_origin_resolve` are not public commands or
aliases. Their exact successor contracts and evidence requirements are recorded
in `successors/t46_spatial_content_operation.json`.
