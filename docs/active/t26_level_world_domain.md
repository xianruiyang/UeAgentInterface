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

## Optional scene-editing profile

`content_graph.export_workspace(..., profile_id="scene_editing")` is an optional
Level projection. The default `llm_authoring` profile remains unchanged. Selection
happens before Actor/Component property read requests are built, so excluded values
and descriptors are not captured merely to discard their output later.

The scene profile retains Actor/Component membership, identity, label, folder,
tags, level ownership, attachment, Actor world transform and component relative
transform. Basic reflected Actor properties include `bHidden`; component properties
include `StaticMesh`, `SkeletalMeshAsset`, `OverrideMaterials`, `Mobility`,
`bVisible`, `bHiddenInGame` and `CastShadow`, where their existing native authorability
rules permit editing. Material overrides keep the native slot-indexed array,
including null entries. It edits component references/overrides, not shared assets.
Built-in native Engine light components also expose `ULightComponentBase::Intensity`
and `ULocalLightComponent::AttenuationRadius`; native PostProcessVolume exposes its
complete `Settings` struct. Edit the required fields while retaining other Settings
members, references and override flags. This is the same reflected-property codec,
not a partial struct or a separate light setter. These additions check the actual
resolved class and declaring property, including initial properties of new components;
a same-named property on an unrelated or Blueprint class is not thereby supported.
PCG authoring state is omitted. World topology and WorldSettings retain their
existing contract; this profile specializes placed Actors and their components.

Use the same directory `import_changes()` workflow. Source custody recovers the
profile from the verified workspace and carries it internally through import and
refresh; callers do not supply an import profile or machine metadata. Omitted
Actor/Component properties are not deletion/reset requests. Only changes in the
projected workspace reach the existing apply path; explicit out-of-profile reflected
properties or PCG state are diagnosed, including initial component properties.
Use a separate full-profile workspace for other lighting parameters, simulation, custom
gameplay variables, PCG, or other properties outside this projection. Native edits
such as Blueprint construction or changing the mesh can still update related values.

Actor/component creation and removal keep their existing membership semantics;
all unrelated rows must remain in the captured collection. Successful refresh keeps
`scene_editing`, including newly created objects. The profile does not weaken
identity/revision validation, transaction recovery, persistence or readback. Complete
recovery/persistence captures can still run internally, so reduced export cost does
not imply an equal reduction in total import/save cost.

For a new Level Actor, query `client.world.actor_candidates(...)` with the saved
World as `target` and the required placement `category`/`query`. In the exported `actors` map, add one keyed item and copy
the selected candidate's readable `name` directly into that item; the map key is
the instance identity. For a real same-name ambiguity, copy the returned short `select`
alongside `name`; discovery remains names-only. Actor choices can distinguish an asset's
`folder`, `class_module` or `factory_module`, using the shortest real namespace needed.
`select.folder` identifies the candidate asset directory, not the placed Actor's folder.
Do not write a nested `candidate` object. Keep placement,
attachment and owner context in their declared item fields. Do not use a
separate authoring-schema command, static template or legacy folder contract.

For an existing Level Actor, `world.component.candidates` uses that Actor's exact
`object_ref` from `content_graph.items`. It shares native component discovery and
property description with other authoring domains, but binds the Level instance
owner. `blueprint.component.candidates` remains a Blueprint/SCS query.

Each captured Actor owns a `level_components` structure with a public `components`
map. Add a key with the returned candidate `name` (and `select` when needed) to
create an instance component; remove an exact captured key to delete it. The
common collection diff lowers these edits into the existing Level lifecycle,
transaction, persistence and workspace refresh. Do not invent owner GUIDs,
write internal operations, or delete component directories. Class/SCS-generated
components remain owned by their class and cannot be removed as instance additions.

`world.component.candidates(..., describe={name, select?, fields?})` returns native
default property descriptors and creation support. A new component row can carry
the selected `description.properties` as `properties` in its first import. Native
asset-backed candidates use their component asset broker. Existing component
properties stay in their ordinary property document; attachment uses the exported
Level attachment shape. A new Actor must first be created and refreshed before
its component collection is available. Independent scene parts still belong in
separate Actors; component support is not a reason to wrap a scene in one container.

Neither `world.actor.candidates` nor `blueprint.component.candidates` currently
accepts candidate `describe`. Their discovery result does not declare editable
initial properties. Use current exported fields/examples for Actor placement;
use the refreshed native component property document for reflected edits. A
selected mesh asset can be placed through its native Actor candidate/factory
without first creating an empty Actor or a scene-container Blueprint.

## Scene context and geometry analysis

`world.scene_context` and `world.geometry_overlap` are independent read-only Query
commands. Both accept the current loaded World's exact `targets` or `regions`
(`min_cm`/`max_cm`), or a `workspace_root` containing an edited `scene_editing` Level
workspace. The latter shares `TryPrepareLevelWorkspace` and
`FPreparedAuthoringImport` with Import: identity, native normalization, upsert,
membership removal, baseline/current rebase and conflicts have one owner. Missing
fields retain their existing import meaning. The proposal overlays additions and
updates and removes deletions in memory; it does not run arbitrary construction
scripts, mutate the live World or clone a map.

The returned `analysis_ref` owns that preparation and snapshot. Either command can
reuse it; only `offset`, `limit` (1–200, default 40) and `object_ref` may change.
Changing the source, scope or tolerance requires a new preparation. A context-only
snapshot can be upgraded to geometry without repeating import preparation.
Current-scene queries require a scope rather than defaulting to a whole-map scan.
Old and new bounds remain separate regions for moves and replacements. The local
index includes components and individual ISM/HISM instances without collision
filtering; large Actors do not collapse to a single Actor bound. Attachments carry
predictable native relative transforms to affected children. Unknown construction,
socket/actor attachment effects and unsupported component geometry are explicit.
Actual WorldSettings/topology edits and unbounded native construction effects mark
`affected_scope_unknown`; an empty predictable region is not a clean scene result.
Their binding conservatively checks loaded-world membership, and cannot certify
uncaptured mutable geometry. Post-apply verification retains an explicit actual
scope review for these edits.
World Partition queries cover loaded cells and report that limit.

Geometry uses immutable StaticMesh source LOD0 or DynamicMesh vertex/triangle
snapshots, shared local BVHs and transformed surface comparisons. StaticMesh source
geometry is not final render LOD, Nanite, animation, WPO or material depth output.
Editor visualization components and native `UDebugDrawComponent` helpers are
excluded explicitly: their debug-container bounds are not scene surfaces. This is
not a blanket exclusion of Editor-only content. Other primitive types remain
listed with an unsupported reason. Broad-phase bounds
select candidates, not verdicts. The kernel canonicalizes exactly coincident
vertices across UV/normal seams and joins adjacent coplanar triangles into complete
surfaces. It reports same-facing `coplanar_overlap`/`duplicate_surface`,
`near_coplanar_overlap`, `surface_crossing`, `touching`, closed-solid island
containment and asset-internal duplicate/intersection facts. Normal shared edges
are not internal defects. Open/nonmanifold/inconsistently oriented/self-intersecting
geometry cannot certify solid containment. Contact/embedded geometry is not
automatically an error; collision profiles and object names do not authorize it.

Area facts include `area_cm2`, each related whole `surface_a_cm2`/`surface_b_cm2`,
and the two fractions. `surface_totals` union coverage on each whole surface across
objects, avoiding repeated area counting. Near-coplanar facts include projection
area and signed plane distance. Line/point facts and containment do not invent
volume percentages. Ratios neither rank design severity nor prove rendered
Z-fighting. Negative/nonuniform transforms are evaluated in world space.

The default result includes compact counts, paged object/fact rows, representation
and coverage, and capture/compute timings. Object rows retain exact identities,
ownership, mesh, effective matrix and bounds. A single merged mesh remains one
component in context; its bound is not a semantic decomposition into local walls
or floors. Geometry facts associate that component with specific surface IDs and
witness positions. `object_ref` retrieves one object's
metadata or facts; full HJSON/property descriptions remain at the existing
content-graph/workspace entrance. Counts and paging flags also apply to surface
totals and coverage. Unsupported, budget-limited, cancelled or stale results are
never equivalent to zero overlaps or a design pass.

Capture is game-thread-only; triangle mathematics runs on one background worker.
A reusable component/instance index is refreshed from a lightweight live inventory
to catch manual changes that bypass UObject notifications. Formal import
preparation and this inventory still have scene-size costs. Geometry/BVH caches
are shared by asset incarnation/revision; unchanged local pair results are reused.
Relevant mutable meshes also bind an actual vertex/topology digest. A newer edit
of the same workspace cancels an older analysis. Resource bounds currently retain
at most 8 snapshots, 64 geometry cache entries, an estimated 128 MiB geometry
allocation, 16 MiB pair results, 500,000 captured vertices per preparation,
100,000 inventory entries, 20,000 scoped entries and 20,000 facts. Limits produce
explicit failure/coverage information; they do not silently truncate a clean
result. Cold assets, many instances, broad edits and shared-asset changes can
still be costly. These bounds are resource protection, not a global import gate.

An optional top-level `analysis_ref` on semantic Level Import validates the edited
workspace, canonical prepared native changes, relevant live objects/assets and
neighborhood membership before mutation. Supported structure edits use the formal
hierarchy/collection path. Unsupported raw manifest edit kinds are rejected by
formal preparation before mutation, not enabled by analysis. A normal persistent
import reloads the World for independent readback; a related snapshot survives an
unrelated reload only after exact identities, measured state, asset dependencies
and neighborhood membership have been rechecked. The package name alone is not
proof that geometry is unchanged.
It does not alter ordinary Import defaults. Native application/save/readback keep
their existing success and recovery contracts. A bound import adds `scene_analysis`
after successful readback: predicted source geometry/placement either matches or
identifies concrete objects requiring current-state reanalysis. It does not claim
to have executed previously unknown behavior in the read-only preview. The old
reference can page this applied verification as a historical snapshot, and cannot
be replayed as a new mutation authorization. Recover interrupted operations using
their existing identity instead of resubmitting mutations.

## Stage ownership, retirement and restoration

Level blockout, art blockout and final-scene instances use the same Level
workspace. Organize scene-content instances under one World Outliner root, then
the three stage folders: `SceneContent/<stage>/<optional nested folders>`.
Every scene-content Actor belongs beneath a stage; region/function/part nesting
is flexible. Tags are additional query aids, not a substitute for this folder
ownership. Keep independently replaceable parts as independent Actors. Shared
gameplay has its own group inside the owning stage, not a parallel root-level
escape from stage classification. When carried forward, retain the existing
instance and references, record its original ownership and change `folder_path`
to the receiving stage before retiring the old stage. There is no need to
duplicate a reusable Content Browser mesh/material asset for each stage.
Folders and tags do not implement visibility, deactivation or loading.

Stage retirement is an edit of the affected instances, not a new command or a
deletion request. Keep their captured Actor/component collection rows and
identities. Temporary display isolation and persistent static retirement differ:

| Native field | Meaning for an applicable component/Actor |
| --- | --- |
| Actor `bHidden` | Hidden in game; not an Editor-folder visibility toggle or a collision/behavior switch |
| Component `bVisible`, `bHiddenInGame` | Rendering visibility and game-hidden state; edit every relevant component, not only a parent |
| Primitive `CastShadow` | Shadow contribution; hiding alone may retain a hidden shadow |
| Primitive `bGenerateOverlapEvents` | Overlap-event participation; does not itself remove blocking collision |
| StaticMeshComponent `bUseDefaultCollision` | Whether the mesh asset supplies collision; explicitly set false when instance collision must override it |
| Primitive `BodyInstance` | Native collision profile, enabled state, object type and channel responses |

For an ordinary static blockout mesh retained off-stage, use the exported fields:
`bHidden=true`, `bVisible=false`, `bHiddenInGame=true`, `CastShadow=false`,
`bGenerateOverlapEvents=false`; select the project's native no-collision profile
and `BodyInstance.CollisionEnabled=NoCollision`. For StaticMeshComponent also set
`bUseDefaultCollision=false` in the same import. With it left true, UE can keep
using the mesh asset's collision even when the authored profile name reads
`NoCollision`. Changing shared mesh collision is not a substitute for this
instance edit. Do not insert fields into component classes that do not expose them.

Collision/overlap/default-collision settings require `llm_authoring`; they are not
in the optional `scene_editing` projection. Preserve the current property type
definitions and unrelated values. Native profile normalization can change
`ObjectType` and `CollisionResponses` alongside `CollisionEnabled` and
`CollisionProfileName`. Record the original collision configuration, including
the default-collision flag, before retirement and consume normalized readback.
Import success can carry warnings: a relevant `readback_mismatch` is not evidence
that the requested retirement succeeded. The SDK recipe is in
[CMD T26](../../../../UeAgentInterfaceCMD/docs/active/t26_level_world_domain.md#stage-management-with-existing-hjson).

Restore recorded values into a current/refreshed workspace for the same objects;
do not import an old complete workspace as a restore operation. Restore fields
changed directly or by native normalization, preserving unrelated later edits.
Do not reset all instances to visible or `QueryAndPhysics`: the original states
can differ. Handle the current replacement instances in the same intended scope
so restoration does not leave both stages active. Removing and recreating an
Actor creates a new identity and belongs to the replacement contract below.

These fields do not suspend arbitrary Blueprint logic, Tick, timers, AI, audio,
Niagara or other lifecycle behavior. `bStartWithTickEnabled` and `bAutoActivate`
are initialization settings, not proof of current deactivation. Use the behavior's
existing owner and supported lifecycle; report any still-active content rather
than claiming a universal dormant state. Old lights, navigation contributions,
baked lighting and other derived data need their own applicable handling; the
static-mesh recipe alone does not prove they have been removed.

Retained instances still load and may retain assets in memory or cooking. Existing
sublevel/Data Layer loading can be used where the map already has suitable
ownership; Editor session visibility/loading, persistent initial configuration
and runtime activation remain separate contracts. Stage classification alone
does not require conversion to World Partition or new streaming infrastructure.

## Replacement through the existing workspace

For the same Actor and the same StaticMeshComponent, edit only the exported
`StaticMesh` and/or `OverrideMaterials` property values. This keeps the existing
Actor/Component target; it does not edit the shared mesh/material asset or replace
the Actor class. Existing references still address those same objects; neither
recreating an Actor with the same label nor creating a similarly named component
provides that guarantee. Preserve the property `type` and `definition`. Material overrides
are indexed by the replacement mesh's slots, not by the old mesh's slot names;
mesh-local bounds, pivot and collision may change even when Actor placement and
identity do not. Use the actual replacement asset and the current component
`BodyInstance` when those differences matter. Blueprint construction logic may
also own a value or reconstruct components; an instance override is not a substitute
for changing that owner or proof of reconstructed component identity. A cleared
override slot uses the replacement mesh's material; retaining an old slot-indexed
array does not establish the intended material mapping.

For one-to-many or many-to-many Actor replacement, add the selected new keyed
Actor rows. For permanent removal, remove only the replaced Actor keys from the **same captured**
`actors` map. The semantic collection diff converts a missing captured key to
remove and a new key to create; no authored `operation` or machine identifier is
needed. Retain all unrelated rows. The component membership map uses the same
captured-key removal rule; `level_world.streaming_levels` has a different omission
contract. For recoverable old-stage content retain the rows and apply
[stage retirement](#stage-ownership-retirement-and-restoration). Deleting an
exported file or Component directory does not remove its object.

New and removed Actors enter the shared import transaction. Do not also edit the
state/properties of an Actor being removed. Replacement Actors have new identities;
references and independent gameplay ownership do not automatically transfer from
an old Actor to a new one. Assign each needed relationship to the part that now
owns its responsibility; a scalar Actor reference cannot become a reference to
several Actors. Preserve independently needed gameplay/interaction owners instead
of copying behavior onto every visual part or creating a scene-container Blueprint.

When new component properties or exact native references are needed, create the
new Actors first, consume the same workspace's refresh and obtain their actual
`object_ref` values. Then configure parts, explicitly rebind known supported
references and retire the old presentation. Reflected Actor-reference changes
on surviving components can share an import with old-Actor removal once the new
targets exist. Other binding owners may require their own sequence; the Level
transaction does not make separate asset imports atomic. Do not present the
intermediate overlap as the completed replacement. Transaction support alone
does not establish visual, collision or gameplay correctness.

### Reference scope and preservation

Use the full `llm_authoring` workspace when finding/editing ordinary reflected
references; `scene_editing` deliberately omits most such fields. Within the
selected map's exported objects, locate exact old Actor/component references and
the actual referring fields, including nested arrays/structs. Read known owners
with `content_graph.properties`. Edit the typed property value, not dependency
reports, definitions, machine metadata or a text-wide replacement of names.
Returned native object paths, captured Actor keys and generated workspace aliases
are different identities; consume the refreshed mapping after membership changes.

`asset.relations` reports on-disk Asset Registry dependencies, not live Actor-field
referencers. Level `content_graph.relations` currently emits Actor membership and
component ownership; it does not derive reference edges from arbitrary reflected
properties. `reference.origin.resolve` resolves origin, not incoming references.
Searching one full Level workspace therefore covers only its exported objects
and values, not all other maps, graph nodes, Sequence bindings, construction logic,
runtime state or name-based gameplay lookups. Inspect affected known owners using
their existing domain entry; report uncovered consumers without claiming a global
reference scan. Native deletion is not a generic reference-remapping service.

Keep existing attachment state when only changing a mesh. The current Actor
attachment apply path uses the exported `parent_managed_id`, not the hierarchy
key or native `object_ref`. It has no general readable-parent selector here;
without a valid public value for a new parent, do not invent an internal identifier
or treat a path as that identifier. Preserve a needed existing logical parent or
identify the unsupported attachment migration separately. Ordinary reflected
`object_ref` rebinding does not validate this different relation type.

The SDK examples and scoped property queries are in
[CMD T26](../../../../UeAgentInterfaceCMD/docs/active/t26_level_world_domain.md#component-properties-and-same-actor-replacement).

## Apply and native state

Level provider mechanics remain domain-owned; common HJSON source resolution,
transactions, persistence and readback are shared. Generated Level units carry a typed
source resolver for their original semantic Actor rows, so candidate errors report the
actual HJSON file and field location instead of the intermediate inline bundle.

The inactive authoring scratch world initializes a native scene so reflected editor
property callbacks can inspect lighting/atmosphere state during preflight. It remains
outside engine world contexts, with audio, physics simulation, navigation, AI and FX
disabled. The same scratch owner releases the world after preparation/readback; native
property notifications are not skipped to work around missing preview context.

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

The World target also exposes `General.level_world.value`. Its
`streaming_levels` object is keyed by existing non-partitioned map package paths.
Each row declares `streaming_class`, `editor_visible`, and `transform` (world
translation in centimeters, rotation in pitch/yaw/roll degrees, scale). New rows
default to `LevelStreamingDynamic`, visible in the Editor, with an identity
transform. Dynamic subclasses additionally expose native `initially_loaded` and
`initially_visible`; omitted settings retain the current value or native class
default. Existing relation classes cannot be changed in place. Set a row to
`null` to unlink that map; omitting a row or writing an empty object is not a
deletion request. Unlinking retains the sub-map asset and its contents.

Actor hierarchy items expose `level`, their owning map package. Set it to the
persistent map or an associated loaded sub-map to move an Actor using UE's native
move operation, preserving its world transform. Native moves may replace the
Actor and its GUID; consume the refreshed workspace afterward. Source and target
map packages participate in the same save/rollback scope. Loaded hidden sub-map
Actors and Components remain editable. Sub-map saves remove and restore the
Editor streaming transform using native Level utilities, so repeated saves and
unlink/relink do not bake that offset into local map coordinates.

Editor visibility is separate from runtime initial flags. UE keeps associated
legacy sub-maps loaded in the Editor; this interface does not promise an unloaded
Editor sub-map while retaining its association. `world.streaming_levels` reports
actual loaded/visible state, Editor visibility, runtime requested flags, and
Dynamic initial flags separately. No PIE behavior is implied by Editor readback.

Ordinary maps become World Partition maps through the native conversion lifecycle
`policy:world-build:partition-convert/v1`, not a Level property update. It writes
`<source>_WP` with `ConversionSuffix` and preserves the source. A blank partitioned
map can instead be created by the existing WorldContext create policy. The conversion
wrapper uses the native UE converter and limits output ownership; see CMD T59 for
preconditions, destination collisions and unsupported custom conversion configuration.

For an existing World Partition map, `level_world.world_partition.streaming_enabled`
uses the native streaming setter. Other WP settings, including the default HLOD
Layer, use their existing reflected WorldPartition properties. Actor spatial
loading, RuntimeGrid and HLOD Layer use the Actor's exported native properties.
Partitioned Actors retain their persistent map ownership. Workspace export loads
and retains WP Actor descriptors for capture, including currently unloaded Actors.
HLOD Layer assets use the Reflection workspace; native HLOD generation and Editor
region-loader lifecycles use the public policies described in CMD T59. The opt-in
`world.partition.include_actor_descriptors` query reports unloaded Actor identity,
external package, membership and loading status without loading Actors for capture.
DataLayerAsset create/update uses the Reflection profile and `uai.world_data_layer.edit/1`.
`level_world.data_layers` is keyed by the existing asset's exact object path; each row
configures `initially_visible` and, for Runtime layers, `initial_runtime_state`.
Missing instances are created by the native Data Layer Editor subsystem. Omissions
preserve state; instance deletion, hierarchy and private/external layers are not
supported by this field. Actor membership uses the exported `DataLayers.DataLayerAssets`
property. The `data-layer-editor-state` session temporarily changes Editor loaded/visible
flags and restores them on close; it does not change initial/runtime state or user
loading preferences. Configuring
these properties alone does not establish generated HLOD content or runtime streaming.
Generated HLOD components remain inspectable, but their native Editor-managed
`bVisible` flag is excluded from persistent authoring and readback comparison.
Ordinary component visibility remains editable. Data Layer Actor preview preserves
reflected intent without scratch native notifications when layer membership is
involved; actual Actor notifications and durable reloaded verification still run.

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
