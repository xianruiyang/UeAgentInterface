# T26 Level / World domain

Use positive-keyword `client.query.command_search(...)` and
`client.query.command_usage(...)` to discover the current Level commands and
their minimal Python forms. Export the
target with `workspace = client.content_graph.export_workspace(...)`; the exported HJSON carries the
editable actor/component fields, enum candidates, identities, transforms,
necessary metadata. Edit only declared fields and import the same directory carrier with
`workspace.import_changes()`. Export returns a managed workspace handle. Successful
semantic directory Import returns `status: succeeded` with `workspace_refresh`;
the refreshed directory contains the standard exported shape and current baseline.
Revisions, hashes and internal verification evidence remain service-owned. The shared
refresh and recovery contract is defined in [T13](t13_uobject_recursive_init.md).

Only model-visible HJSON is editable. Keep `.uai/workspace.hjson` unchanged;
its inventory and hashes are the machine-owned baseline from which Import recovers the target
and concurrency context and recomputes edited-unit hashes and the current semantic fingerprint.

If an Actor or Component is absent from the export, query the native creation
surface with `client.world.actor_candidates(...)` or
`client.blueprint.component_candidates(...)`. In the exported semantic structure map, add one keyed item and copy
the selected candidate's readable `name` directly into that item; the map key is
the instance identity. For a real same-name ambiguity, copy the returned short `select`
alongside `name`; discovery remains names-only. Actor choices can distinguish an asset's
`folder`, `class_module` or `factory_module`, using the shortest real namespace needed.
`select.folder` identifies the candidate asset directory, not the placed Actor's folder.
Do not write a nested `candidate` object. Keep placement,
attachment and owner context in their declared item fields. Do not use a
separate authoring-schema command, static template or legacy folder contract.

Level provider mechanics remain domain-owned; common HJSON source resolution,
transactions, persistence and readback are shared. Generated Level units carry a typed
source resolver for their original semantic Actor rows, so candidate errors report the
actual HJSON file and field location instead of the intermediate inline bundle.

Actor transforms and their component properties can be edited in one import.
Dependencies resolve the actual owning Actor even when the Actor and Component
use different exported selectors; the component snapshot follows its owner edit.
Actor/world and component/parent-relative transform snapshots declare their nested
spatial types to the common semantic comparator. Verification and saved/restored
readback accept native spatial roundoff and equivalent rotations; other fields and
collection identities retain their original equality rules. Authoring values do
not need manual rounding or readback-value substitution.

The same Level workspace exposes editable native `AWorldSettings` properties in
the World target's `properties.hjson`, grouped by native categories. Use the exported names, categories and typed values; UE's
GameMode Override is the native `DefaultGameMode` class property. World settings
use the existing recursive property codec, reflection apply, package transaction
and persistent readback. Unedited fields remain unchanged; no dormant typed World
mutation or separate WorldSettings workspace is required.

For PrimitiveComponent `BodyInstance`, selecting a named collision profile uses
UE's native profile values for unedited ObjectType, CollisionEnabled and response
channels. Explicit changes are compared with the exported baseline: values that
agree with the chosen profile keep that profile, while conflicting custom values
use native custom-collision semantics. Level and Blueprint component templates
share this normalization through the common reflected-property operation owner,
using the actual instance/template baseline and an isolated preview. The complete
normalized struct remains available for readback diagnostics, whose consistency
differences are warnings. An unchanged exported response array is context, not a
request to restore old channels after selecting a profile. When the response array
itself changes, all explicitly retained entries are applied, including values equal
to the old baseline; omitted entries retain the selected profile's defaults.

After all Actor/Component input updates, Level imports publish the final bounds of
new or resized NavMeshBoundsVolume actors, process the world's pending native
navigation updates, and finish submitted generation work on registered navigation
data before capturing the save inventory. Automatically created navigation actors
and their completed updates are saved together, so reload does not recreate their
identity or require regeneration of an intermediate navigation state. Unchanged
bounds are not resubmitted for unrelated edits. The import retains the configured
navigation update policy and does not force a full navigation rebuild. This boundary
applies to directory and inline imports, including caller-controlled map saves.

Level Actor items also expose read-only `geometry_components` for attached
InstancedStaticMesh and SplineMesh components, including generated transient
components that are intentionally absent from editable workspace items. ISM
`instances` contain world translation, rotation (pitch/yaw/roll in degrees),
scale and instance index. SplineMesh `start/end` and tangents use world space
and centimeters; `world_transform` records the component transform.
`offsets` are separately tagged `spline_cross_section` and are not included
in the reported centerline endpoints. These observations are not authoring
properties and must not be copied into Level workspace structures.

Spline Component `SplineCurves` uses the shared `type: spline` property codec,
for both Blueprint component templates and Level instances. Its value contains
`schema: "uai.spline.points/1"` and a `points` array. Each point declares an
increasing `input_key`, `type`, local-space `position: [x,y,z]`, quaternion
`rotation: [x,y,z,w]`, and `scale: [x,y,z]`. Positions use centimeters.
Supported point types are `Linear`, `Constant`, `Curve`, `CurveClamped`, and
`CurveCustomTangent`; the last also declares local `arrive_tangent` and
`leave_tangent` vectors. Edit the exported property rather than introducing a
Level-specific spline structure. The common apply owner synchronizes native
spline state, marks points edited and rebuilds derived curves; rollback restores
the previous points and edited flag. Reparameterization tables and automatic
tangents are derived, not authoring input. Native closed-loop properties remain
separate from this points value.

Embedded RuntimeFloatCurve properties inside component structs use the shared
curve value contract. Follow their references in `definitions.hjson` for legal
interpolation, tangent and weight options and the key template; equal contracts
are defined once. Edit the curve under `channels.value`, preserving its declared
shape. The same definition exposes `value_templates.embedded` and
`value_templates.asset_reference` in either state. To switch forms, replace the
whole curve value with the chosen template, then supply its keys or a nonempty
`asset_reference.asset_path` pointing to a CurveFloat asset. Assigning a reference
preserves the inactive embedded curve and does not edit the referenced asset;
applying an embedded value clears the external reference after validation.
Coordinates use the native property's meaning: AudioComponent air-absorption
curves map normalized distance to frequency, so their coordinate is not seconds.
Native edit conditions still apply, including `bOverrideAttenuation` for
`AttenuationOverrides`.

Existing non-World-Partition map updates perform final readback within the shared
durable transaction. When editing the active map with unsaved changes, a rejected
update restores the prior in-memory state and the original disk file separately.
This lifecycle coverage does not extend to map creation or World Partition maps.

Native construction-script component replacement is resolved through the current
World/Actor/component identity for actual readback. Such component updates use
the existing persistent memory checkpoint for failure recovery rather than
writing rollback values through a replaced component's stale addresses.
Multiple property edits on one existing construction-script component are copied
before one whole-object native notification. Readback then resolves the current
component, so reconstruction cannot leave later edits on a retired instance.

For edited actors owning editor PCG components, the Level provider compile phase
settles native input refresh before staging the map. PCG's refresh, generation
and cleanup queues must all finish; map reload is not a substitute for refresh.
PCG-managed generated components and its non-user-managed World service Actor
are not editable authoring source. Generated meshes remain observable through
the Actor's read-only geometry projection.
The persistent path ends its already-discarded Undo recording before compile,
because native PCG execution pauses while an editor transaction is recording;
the durable memory/disk checkpoint remains active through final readback.
REQ30 covers synchronous Static Mesh Spawner loading. This does not establish
completion for arbitrary PCG graphs waiting on unrelated editor async callbacks.

Use `client.content_graph.diagnostics(...)` for explicit read-only diagnostics;
`ueagentinterfacecmd` exposes the same command-search, exact-detail and execution surface.
T13 remains the shared recursive initialization owner: do not use or invent `level_apply_folder`
or another Level-specific HJSON ingress.
