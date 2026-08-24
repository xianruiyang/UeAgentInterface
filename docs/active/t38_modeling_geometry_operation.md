# T38 ModelingGeometry Operation Authority

The active `modeling-geometry` contribution owns exactly the 32 mesh attribute,
collision, primitive, topology and Modeling Mode command identities assigned to
`T38-MODELING-GEOMETRY-OPERATION` in `UAI_COMMAND_TASK_INVENTORY.json`. Every
frozen contract reports `modeling-geometry-owner`; no namespace or catch-all
fallback adds commands.

The exact handlers require `uai:operate`, add only the private
`uai:modeling-geometry:write` mechanics scope, and forward one request to the
common OperationService. The ModelingGeometryEffect adapter remains the sole
owner of the real Unreal Engine geometry differences. It is an internal
mechanics dependency, not a second public route, registry, handler, contract,
test or documentation authority.

Success and failure remain command-specific. The common lifecycle performs
schema validation, planning, ticket binding, native execution, settle,
persistence, independent readback and rollback or `state_unknown` handling as
required by the selected descriptor. No success is emitted before all required
stages finish. A prewrite rejection leaves no residue, and `state_unknown` is
not safe for automatic replay.

`mesh.modeling_session` is the single public successor for Modeling Mode
lifecycle, active-tool observation, tool properties/actions, mesh selection,
save, actor-mesh replacement and surface snapping that are not already owned by
an exact current mesh command. Its required `action` discriminator is one of
`activate`, `observe_tool`, `start_tool`, `set_tool_property`,
`invoke_tool_action`, `accept_tool`, `cancel_tool`, `observe_selection`,
`set_actor_selection`, `set_selection_mode`, `clear_mesh_selection`,
`select_via_screen`, `select_via_world_ray`, `replace_actor_mesh`,
`save_mesh_asset`, or `snap_to_surface`. The optional `target`, `input`,
`context`, and `projection` objects carry only action-specific data. Unknown
actions and missing required action inputs fail explicitly; historical
`modeling_*` IDs are not aliases.

## Public SDK and CLI

The final manifest projects all 32 identities into the default `mesh`
namespace. Callers import only `uai`, connect from one complete Connection TOML,
and call generated methods such as `client.mesh.primitive_box_create(...)` and
`client.mesh.topology_mirror(...)`. Generic `client.execute(...)`, exact
`client.execute_exact(...)`, and `uai-cli exec` reuse the same generated catalog,
Client, Protocol and Transport. No T38-specific Python module, command/hash
table, parameter mapper, retry, persistence, rollback or readback path remains
authoritative.

```python
result = client.mesh.modeling_session(
    action="observe_tool",
    target={},
    input={},
    context={},
    projection={},
)
```

```text
uai-cli --connection connection.toml exec mesh.primitive.box.create --params-json {"anchor":"base_center","placement":{"semantic_frame_id":"uai.world","write_frame_id":"ue.world","read_frame_id":"ue.world","verify_frame_id":"ue.world","unit":"centimeter","translation":[125,-50,30],"rotation_quaternion_xyzw":[0,0,0,1],"scale":[1,1,1],"basis_evidence":"UE left-handed Z-up centimeters","conversion_chain":"identity"},"size_cm":[120,80,60],"subdivisions":[2,2,1],"target_ref":"UAI/StrictV2/T38/Box_Default_r58"}
```

Use `client.query.command_search("create mesh box", limit=5)` and then
`client.query.command_detail(command_id, contract_hash)` to obtain the exact
schema, named SDK/CLI forms, owner, failure catalog and tutorial references.
Search is the only fuzzy step; execute the selected ID and hash unchanged.

## Persisted Modeling artifact readback

The production Modeling persisted-state oracle does not unload or reload the
source asset that the user may still have open. For a `static_mesh_asset`
carrier it consumes the exact package-header artifact owned by the operation
receipt, remaps that `.uasset` into a unique transient package namespace, and
runs the same semantic state observer against that isolated instance. A
successful canonical C9 outcome therefore carries an evidence identity that
starts with `modeling:persisted-package-instance:`. Re-resolving the live source
object is not sufficient persisted-readback evidence.

For world-package carriers the same production oracle consumes the Map and any
External Actor artifacts owned by the operation receipt, loads them into an
isolated transient World, and unloads that snapshot after observation. An
existing `dynamic_mesh_actor` baseline carries a durable Actor GUID and resolves
the exact snapshot Actor by that GUID; primitive profiles resolve their unique
target label through the same isolated World catalog and state reader. A
successful world-carrier C9 outcome starts its evidence identity with
`modeling:persisted-world-instance:`. Neither branch falls back to a live source
object in the default production composition.

An explicitly supplied persisted reference resolver remains owned by the
custom or test composition that supplied it. The production runtime leaves
that override unset and uses the artifact-aware oracle as the single default
owner. Forged and stale carrier revisions are rejected before durable mutation.

The current active-registry product proof covers 23 non-normals command
identities on owned `UStaticMesh` assets: the six UV, material-slot and
collision commands above, and all 17 `mesh.topology.*` identities. It includes
exact-channel UV semantics, an explicit target-local projection frame and
dimensions, exact slot name/material reference readback, selected-versus-
unselected triangle material IDs, exact BodySetup shape-class/count readback,
isolated C9 readback, independent source-package unload/reload and cleanup.

The topology proof distinguishes uniform and PN-triangle subdivision, all three
simplify target modes, fixed versus free open boundaries, remesh density,
preserved versus released sharp normal seams, mirror and plane-cut policies,
selection-driven duplicate/extrude/inset/offset/push-pull/bevel/fill/weld
semantics, and boolean/mesh-cut/trim/voxel-boolean operand modes. Selection
operations use real persisted element identities. Vertex-normal displacement
precomputes one-ring normals before region separation. A shared operand carrier
reader accepts the same closed `ADynamicMeshActor | UStaticMesh` union promised
by planning; the shared product runner proves target revision advancement and
persisted target geometry while the operand file, geometry and revision remain
unchanged.

Voxel boolean maps `voxel_size_cm` to the operator's major-axis voxel count,
applies the operator result transform before target writeback and enforces a
maximum of 256 voxels per major axis. Finer requests fail with
`modeling_geometry_voxel_resolution_limit_exceeded` before carrier mutation.
The public rejection proof verifies exact errors and unchanged file hashes,
dirty flags, live/reloaded geometry and revisions for both this limit and an
insufficient weld tolerance. Simplify, remesh, selection, operand and voxel
policies remain owned by shared native helpers rather than test-only adapters.

For `ADynamicMeshActor`, simplify now solves on a transient candidate and commits
only after its hard target and boundary policy are satisfied. A shared geometric
boundary fingerprint is consumed by both the native precommit gate and the
ExistingEdit persisted-state oracle. An unreachable preserved-boundary count
fails atomically with `modeling_geometry_simplify_target_unreachable`; it leaves
the live mesh and revision unchanged and creates no durable World artifact.

The same public World carrier now covers both collision command identities.
`mesh.collision.simple.generate` has persisted evidence for aligned boxes,
minimum-volume shapes, capsules and spheres, plus one `replace_existing=false`
case that preserves a seeded sphere while appending a box. Convex collision
covers single-hull and two-component decomposition. The product oracle compares
aggregate kind/count, mesh-matching local bounds, unchanged geometry, advanced
revision, isolated C9 and source-World reload; it deliberately does not freeze
GeometryScript hull vertices, decomposition ordering or automatic shape choice.

This is representative command-by-command evidence, not an exhaustive
parameter Cartesian product or a Python SDK end-to-end proof of every command.
The DynamicMeshActor normals consumer now has public persisted-World evidence
for face, area-angle-weighted and 30-degree angle-threshold modes, plus a mixed
smooth/split-edge `preserve_existing_hard_edges=true` case whose initial overlay
contains deliberately wrong but valid directions. A shared final-carrier normal
observation verifies complete finite directions, mode-specific direction and
overlay topology, unchanged mesh topology/bounds, advanced canonical revision,
isolated C9, source-World unload/reload and cleanup. This closes those selected
carrier/mode dimensions only; it does not prove an exhaustive normals parameter
matrix or transfer the DynamicMeshActor oracle to StaticMesh.
Two active-registry `ADynamicMeshActor` topology consumers now prove receipt-owned
persisted-World C9 evidence, source-World unload/reload, exact Actor recovery and
cleanup. `mesh.topology.subdivide` proves twelve-to-forty-eight-triangle
persistence. `mesh.topology.simplify` covers ratio, tolerance and count targets,
open-boundary preserved/free policies, stable source/reload revisions and the
atomic unreachable-target rejection above.
All seven `mesh.primitive.*.create` identities now have their own public-product
verticals through the same receipt-owned World oracle. Box and ramp-corner first
proved both native generator families with non-unit rotation and scale; cylinder,
plane, ramp, sphere and stairs then passed as one bounded expansion. Every case
checks its unique target label, parameterized local bounds, isolated C9 evidence,
ordinary external reload and owned cleanup.

Package-instanced Worlds do not initialize transient `ComponentToWorld` for this
read-only snapshot. Primitive persisted readback therefore compares the serialized
authored relative transform of its unattached root component; it does not initialize
the snapshot World or weaken the world-space placement contract. Attached actors
keep the normal Actor-transform path. These seven create verticals, two topology
consumers, two collision command identities and the selected normals matrix do
not prove other DynamicMeshActor commands, complex collision quality, an
exhaustive parameter matrix, or Python SDK end-to-end execution.
