# T38 ModelingGeometry Operation Authority

The `modeling-geometry` contribution owns its mesh attribute,
collision, primitive, topology and Modeling Mode command identities assigned to
the domain. The central lifecycle policy removes HJSON-owned material-slot and
persisted pivot mutations from the normal active surface. Every
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

`mesh.modeling_session` owns only Modeling Mode lifecycle, active-tool
observation, tool properties/actions, session-native actor selection, mesh
selection mode and screen selection, and mesh save. Its required `action`
discriminator is one of
`activate`, `observe_tool`, `start_tool`, `set_tool_property`,
`invoke_tool_action`, `accept_tool`, `cancel_tool`, `set_actor_selection`,
`set_selection_mode`, `clear_mesh_selection`, `select_via_screen`, or
`save_mesh_asset`. Read selection through `world.selection`; use
`mesh.element.select_by_ray` and `world.object_snap_to_surface` directly for ray
selection and surface snapping. Actor-mesh replacement is expressed as HJSON
desired state. The optional `target`, `input`,
`context`, and `projection` objects carry only action-specific data. Unknown
actions and missing required action inputs fail explicitly; historical
`modeling_*` IDs are not aliases.

## Public SDK and CLI

The final manifest projects active identities into their generated namespaces.
Callers import only `uai`, use `uai.connect_project()` for the local project,
and call generated methods such as `client.mesh.primitive_box_create(...)` and
`client.mesh.topology_mirror(...)`. Generic `client.execute(...)`, exact
`client.execute_exact(...)`, and `ueagentinterfacecmd exec` reuse the same generated catalog,
Client, Protocol and Transport. No T38-specific Python module, command/hash
table, parameter mapper, retry, persistence, rollback or readback path remains
authoritative.

```python
result = client.mesh.modeling_session(
    action="observe_tool",
)
```

```text
ueagentinterfacecmd exec mesh.primitive.box.create --params-json '{"anchor":"base_center","placement":{"semantic_frame_id":"uai.world","write_frame_id":"ue.world","read_frame_id":"ue.world","verify_frame_id":"ue.world","unit":"centimeter","translation":[125,-50,30],"rotation_quaternion_xyzw":[0,0,0,1],"scale":[1,1,1],"basis_evidence":"UE left-handed Z-up centimeters","conversion_chain":"identity"},"size_cm":[120,80,60],"subdivisions":[2,2,1],"target_ref":"UAI/StrictV2/T38/Box_Default"}'
```

For unknown command names or parameter shapes, follow [public command discovery](../../../../UeAgentInterfaceCMD/docs/PYTHON_SDK_AND_CLI.md#command-discovery).
Search matches only positive tokens from command IDs, registered keywords and
registered synonyms; call the selected generated method with business fields.
Existing-carrier commands accept `target_ref`; binary operations also accept
`operand_ref` and its transform. Target and operand revisions are derived and
frozen by the server owner and are neither public parameters nor result fields.

Boolean verification derives the expected geometry from the captured target,
operand, operand transform and options through the same isolated native Boolean
kernel used by the edit. Independent persisted readback compares the complete
component geometry multiset, including oriented triangles/material IDs and
component bounds. It does not require a closed result, a change in bounds, or
growth along an arbitrary axis. Geometry counts are projected on both sides of
verification so a mismatch includes concrete differences. Existing component
observation precision remains 0.1 cm for shape and 0.01 cm for bounds; the
StaticMesh expectation uses MeshDescription float-position precision.

## Bundled metric material

New `mesh.primitive.*.create` outputs use
`/UeAgentInterface/Materials/M_UAI_MetricGrid.M_UAI_MetricGrid` by default.
All seven primitive identities accept optional `color: [r,g,b]` (linear RGB,
each channel in `[0,1]`) and `material_ref`. The latter explicitly replaces
the default; `color` with a different material is rejected rather than ignored.
No new command is introduced. Existing-carrier geometry edits retain materials.

The first `voxel_grid.apply()` of a newly created grid uses the same default.
`apply(color=[...])` changes its per-object color, and `material_ref` retains its
existing override meaning. Editing a previously authored grid preserves its
material unless explicitly replaced. A subsequent grid revision retains the
initial material origin, including after its bounds change.

Modeling Mode `start_tool` with auto-accept, and `accept_tool`, apply this
default to newly selected output mesh actors with no material or an engine
default material. Explicit tool materials and existing selected actors are
preserved. Their action input may supply `material_ref` and `color`; a custom
material's color belongs to that material's own parameters. This does not alter
the standalone UE Modeling UI or every third-party tool's own defaults.

The grid uses object-local axes, physical world-centimeter spacing (10 cm
minor / 100 cm major), and smooth triplanar projection. On first assignment,
the actual mesh-local bounds minimum is captured once; later edits do not
silently recenter the ruler. Curved surfaces provide projected scale references,
not an arc-length ruler, and may show intersecting lines in projection blends.

Component **Custom Primitive Data Defaults** persist RGB at indices 0..2,
an initialized marker of 1 at index 3, and the fixed local origin in centimeters
at indices 4..6. Public Level HJSON may edit these reflected component defaults
after creation; preserve the marker/origin when changing color. Runtime-only
custom data and transient MIDs are not the persistence mechanism. Voxel apply
includes these defaults in its existing independent saved-package readback and
restores them with the prior material on rollback.

The master is a plugin-owned content asset. Keep the plugin's `Content/`
directory when installing or distributing; a missing default produces a clear
resource error instead of silently displaying an unrelated fallback material.

## Persisted Modeling artifact readback

The production Modeling persisted-state oracle does not unload or reload the
source asset that the user may still have open. For a `static_mesh_asset`
carrier it consumes the exact package-header artifact owned by the operation
receipt, remaps that `.uasset` into a unique transient package namespace, and
runs the same semantic state observer against that isolated instance. A
successful canonical C9 outcome records hidden persisted-package evidence.
Re-resolving the live source object is not sufficient persisted-readback
evidence, but that machine identity is not projected to the ordinary result.

For world-package carriers the same production oracle consumes the Map and any
External Actor artifacts owned by the operation receipt, loads them into an
isolated transient World, and unloads that snapshot after observation. An
existing `dynamic_mesh_actor` baseline carries a hidden stable actor locator and
resolves the exact snapshot Actor with it; primitive profiles resolve their unique
target label through the same isolated World catalog and state reader. A
successful world-carrier C9 outcome records hidden persisted-world evidence.
Neither branch falls back to a live source object in the default production
composition.

An explicitly supplied persisted reference resolver remains owned by the
custom or test composition that supplied it. The production runtime leaves
that override unset and uses the artifact-aware oracle as the single default
owner. Changed carriers are rejected by server-owned baseline and prewrite
checks before durable mutation; callers do not supply carrier revisions.

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

Voxel boolean maps `voxel_size_cm` to the operator's major-axis voxel count and
applies the operator result transform before target writeback. The computed resolution must be finite and fit the
native integer representation. Insufficient weld tolerance remains a geometric
failure with unchanged carrier state. Simplify, remesh, selection, operand and voxel
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
persistence at level 1. `levels` is passed to the selected native tessellator:
both UE 5.6 `uniform` and `pn_triangles` use `FUniformTessellate` once and produce
`(levels + 1)^2` triangles per input triangle. PN subsequently displaces the
vertices; its header's recursive wording does not describe repeated four-way
subdivision in this version. The shared ExistingEdit oracle for StaticMesh and
DynamicMesh carriers compares the independently persisted triangle count; the count is
also a named verification field so a mismatch reports expected and actual values.
Level-1 evidence alone cannot distinguish this rule from an incorrect `4^levels` rule.
`mesh.topology.simplify` covers ratio, tolerance and count targets,
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
