# T06 non-HJSON Query migration

## Active ownership boundary

The `query` command contribution publishes its non-HJSON read routes through `query-command-owner`. They join the one process-level immutable command registry and exact dispatch owned by the active command composition; the contribution does not create a private command registry or a second public route. Provider authority is separately frozen by `DomainCapability`.

The inventory is:

- asset state: `asset.collection`, `asset.registry_record`, `asset.relations`, `asset.sample`, `asset.validation`;
- configuration and Editor state: `config.resolved_value`, `config.section`, `editor.dirty_resources`, `editor.open_assets`;
- content-graph state: `content_graph.summary`, `content_graph.structure`, `content_graph.items`, `content_graph.relations`, `content_graph.properties`, `content_graph.diagnostics`, `content_graph.compound`;
- Landscape state: `landscape.height.read`;
- project state: `project.platform`, `project.settings`, `project.summary`, `project.validation`;
- reference state: `reference.origin.resolve`;
- world state and runtime probes: `world.capsule_path`, `world.capsule_sweep`, `world.data_layers`, `world.hlod_assignments`, `world.mesh_closest_vertex`, `world.mesh_vertex_position`, `world.nav_connectivity`, `world.nav_find_path`, `world.nav_project_point`, `world.nearby_shapes`, `world.overlap`, `world.partition`, `world.partition_health`, `world.physics_component`, `world.physics_constraint`, `world.ray_trace`, `world.selection`, `world.streaming_levels`, `world.summary`.

Each public ID selects a descriptor whose ID is `uai.query.<mechanics_command_id>`. A descriptor may use a more specific capability ID such as `asset.collection.read`; route-to-descriptor identity is therefore proved by the exact descriptor ID, Query operation kind, query binding and uniqueness, not by forcing the capability ID to equal the public route or by freezing a command count.

The former public `asset.registry_relations`, `asset.properties`, `asset.summary`,
`world.actors`, `world.actor_components`, `world.content`, and `world.property`
identities are dormant. `asset.registry_relations` remains an exact compatibility
identity but reuses the same Asset Registry mechanics and projects only its
historical depth-one string view; ordinary discovery and generated named bindings
expose only `asset.relations`. Persisted asset and World structure, items, summary
and editable reflected properties belong to HJSON export and the ContentGraph query
family. `query.content.search` is discovery-only and returns readable names with the
shortest necessary readable scope; it does not return detail-routing identities or
machine selectors. Asset and ContentGraph Query targets, and shared HJSON export,
resolve these names through the same current ContentDiscovery service before
entering the existing exact-target owner. Search scopes each selected name against
the full current same-name group so filtering does not change its meaning.
Existing exact paths remain accepted. Creation-candidate queries for an existing
asset use the same reference resolver: Blueprint nodes/components, Material,
Niagara, AI, PCG, UMG, Rig, Audio, World Actors, and Timeline tracks accept the
current content-search name, including any returned existing-object scope.
The create-only Animation Blueprint query still requires a future package.object
path; AssetFactory discovery has no existing-asset target.

## Request, mechanics and result

Every request enters `/api/uai/command`, passes the exact command contract, and is dispatched once to the Query handler. The handler requires `uai:query`, preserves the exact command ID, canonicalizes the supplied parameter object, and delegates once to `IUeAgentQueryService`.

Commands that address one object require `target`. `input` contains only descriptor-specific query fields. Registration validates `{}` against the actual compiled input contract to decide whether omission is legal, including composition branches; it does not infer this from only a root `required` array. `context` selects an allowed current state plane and execution context, and `projection` controls fields and provenance. The service does not use HJSON and does not perform alias guessing.

The production Query composition creates the immutable descriptor registry once and returns it beside the runtime service. Runtime execution and command registration consume that same registry instance. Registration maps every route to its exact descriptor and projects the descriptor `input_schema` into the frozen command field `SchemaJson`; missing, duplicate, non-Query, unbound or uncovered mappings fail composition instead of falling back to an opaque object. `ueagentinterfacecmd describe <command-id> --parameter input` exposes that projected input schema without a per-command tutorial table. The Contract/Discovery example projector supports `const`, `enum`, primitive, object, array and `oneOf`/`anyOf` candidates, and validates each composed candidate against the complete contract. Only validated literals enter minimum examples; when no literal can be constructed, Query discovery keeps the required named input variable rather than disabling the command. Identity placeholders describe input shape and must be replaced with actual discovered identities. `query.command.usage` preserves generated object literals and named keyword arguments.

For settings discovery, call `project.settings` with `input.editor_project_capability="project_settings.index"`; optional `input.parameters.container` narrows the index to `Editor` or `Project`. Its sections provide actual `section_id` and `settings_object_path`. Read a selected page with capability `project_settings.page` and `parameters.section_id`; its public `observation.properties` preserves native `property_path`, `kind`, authoring-coded `value` (including `literal`, `selected` and nested collections), and `availability`. Both calls are read-only; output paths and write flags do not create page files. Canonical `input={"section_id":...,"field_ids":[...]}` resolves that same registered page and reads only selected values under `fields`; omitting `field_ids` deliberately returns only `property_count`. Optional `include_defaults=true` adds selected `defaults` as UE text values, or `null` when the common property readback has no independent default; it never substitutes the current settings CDO as its own default.

For example, `asset.relations` exposes `input.required=["direction"]`, the exact `dependencies|referencers|both` enum, and optional `relation_kinds` and `package_scope`, with this executable minimum shape:

```text
client.asset.relations(target, {"direction":"dependencies"})
```

Its single `relation_graph` state entry contains stable Asset Registry identity plus package/object identity when available, token-compact node aliases, actual source-to-target edges, traversal direction, depth, path, cycle, semantic relation properties, redirector facts and explicit Registry completeness. It is based on the on-disk Asset Registry and does not claim live runtime references.

The production service composes the audited Asset/Project, World, ContentGraphRead, and Landscape descriptor registries. It resolves every provider from the cached frozen process `DomainCapability` authority at execution time; it does not construct or retain a local provider registry. Real native hooks execute on the game thread with the current Editor/World context, a resource accounting tracker, and the shared response projector. They are read-only and do not save or mutate assets. Projection returns the complete selected result and is not rejected because an estimated total token count is large.

Content-graph reads use one staged owner. `content_graph.structure` discovers readable graph/container names without expanding nodes, relations, properties, or diagnostics. `content_graph.items` selects current items with `items`/`item_kinds` and requests their exposed top-level keys with `fields`. Dynamic `pin_interface` is omitted by default and returned only when explicitly requested for selected nodes. `content_graph.compound` is the explicit multi-facet batch form, not the default discovery call.

TextureGraph node titles are projected through the same native-identity ordered
alias allocator as HJSON. Two nodes with the same title have distinct public
names such as `IfThenElse` and `IfThenElse#2`; use the returned name in `owners`
or `items`. Query projection does not rename native nodes or change their
authoring identity. Genuine unresolved/ambiguous names still fail explicitly.

For a known Level property, pass the current public `owners` and requested
`fields` to `content_graph.properties`. The existing hierarchy selector resolves
Actor/Component owners before the native snapshot captures those fields; it does
not first materialize every reflected value in the Level. `content_graph.items`
uses the same native topology owner, retaining public identity and requested
geometry without materializing unrequested ordinary property values. The
original composition identity/order and readable alias allocator supply names
directly; the query does not build an authoring Model or JSON aggregate for them.
Partitioned worlds retain loaded Actor references through the last selected
getter. Their complete namespace loading does not require complete property
capture; those references are released before pure result projection.

Requested properties report their current values, including inherited/class-default
values; a value equal to its default is not an absent property. `include_presence`
adds authored-presence information when requested. The legacy `include_defaults`
input remains accepted for compatibility and does not select an overrides-only
view or change which fields are captured. An explicit true value now reports
`content_graph_legacy_defaults_option` as a warning and still returns the same
current values. The content-graph business projections do not expose the generic
projection defaults/details/provenance metadata. Explicit true options report
`content_graph_metadata_projection_unavailable` warnings and preserve successful
current-value queries; no unused metadata projection is made for those options.
Actual authoring consumers retain their required type, definition and presence
capture; the query-specific path does not normalize unused authoring descriptors.

Field projection checks the entire selected item set before producing entries.
Incompatible fields are reported by item kind and actual field contract, with
readable affected-item locations in `related_sources`. A failure's `choose`
contains compatible `items`/`containers` selections derived from the same capture
when publicly addressable items exist; keep the requested fields when refining
the selection. Failure does not return partial entries or silently omit
incompatible items. It does not mean every item in the asset lacks that field.

The process composition binds World reads to the current Editor World. If the
Editor changes maps, the process host invalidates the cached World runtime and
rebuilds the World-backed handlers before resolving the next command; a
handler composed for the previous map is not reused against the new map.

`asset.registry_record` returns object, class and package identity by default.
`include_tags=true` adds the native Asset Registry tag map; `include_chunk_ids=true`
adds its integer chunk array, including an empty array when no chunks are assigned.
Omitted/false options do not expand those fields. Tags remain native strings.
Public query projection preserves explicitly returned business arrays/maps even
when empty; a missing optional field remains absent. This applies to nested and
record-array results as well as ordinary records.
`asset.collection` without selected `field_ids` retains the native field/count
inventory under `fields`; recursive public projection must preserve that inventory.

`world.mesh_vertex_position` and `world.mesh_closest_vertex` share component mesh
reading for StaticMesh (source LOD 0) and DynamicMesh. Target the actual component,
not its Actor or asset. `vertex_index` is the zero-based position in ascending live
vertex-ID order; it is local to the current topology. Positions are transformed
through the component into the requested world-space observation.

`world.capsule_path` keeps its default payload as `{"blocked_segments": N}`.
It sweeps a world-vertical capsule through each input segment in `step_cm`
substeps, stops at the first blocker within that segment, then visits the
remaining input segments to complete the count. A blocked path is a successful
query result, not a query error. Existing input validation and channel defaults
remain unchanged.

Optional `include_first_hit` and `trace_complex` both default to `false`.
When `include_first_hit=true`, the same already-computed native hit is published
as `first_blocking_hit`; no additional collision query is performed. The field
is explicitly `null` when no segment is blocked, and is omitted when the option
is omitted or false. A non-null record contains:

| Field | Meaning |
| --- | --- |
| `segment_index` | Zero-based input segment joining `points[i]` and `points[i+1]`; the earliest blocked segment in input order, then its first blocked substep. |
| `target_ref` | Native hit Component object path, or null if the native hit has no Component. This is not a ContentGraph/HJSON selector or an identity for an internal mesh wall. |
| `shape_location` | Native swept capsule center at the hit. |
| `impact_point` | Native impact point. |
| `start_penetrating` | Native initial-penetration flag. For an initial overlap, the reported positions do not establish a precise contact surface. |

Both positions use the existing spatial-value shape with `frame_id="ue.world"`,
`unit="centimeter"` and three `components`. The result is the first blocker in
path order, not all blockers or the globally nearest object. `trace_complex`
selects the same native collision query's representation; asset collision flags
still determine the available geometry. A complex-query pass does not establish
that the Pawn's actual movement representation can pass, and static sweeps do
not prove navigation, step-up, floor support or runtime movement.

WorldRead remains the sole mechanics owner. Its canonical nullable observation
and the shared public projection preserve the requested null; other optional
fields retain their existing omission behavior. `capsule_sweep` remains useful
for its multi-hit or direct single-segment query, and `overlap` for occupancy.
Do not append either solely to recover the same first hit already returned by
`capsule_path`.

`editor.dirty_resources` owns a read-only dirty-resource observation rather
than reusing the open-asset set. For `map` resources it observes loaded World
packages, their loaded map-build-data packages, and loaded External Actor
packages. For `asset` resources it observes dirty persistent packages in every
mounted content root, including plugin content; it does not use an asset-class allowlist.
Configuration kinds remain separate. `resource_kinds` filters these
sources before projection. The query does not call an Editor save helper whose
enumeration would itself mark a newly created World dirty. Dirty discovery,
explicit resource save and safe-exit share the EditorResources persistence owner.

`world.nav_find_path` and `world.nav_connectivity` both expose
`allow_projection_failure`. It is meaningful only when `project_to_nav` is
true; supplying it, or a projection extent, while projection is disabled
fails with `world_read_navigation_projection_policy_invalid` instead of being
silently ignored.

`world.selection` freezes the current actor selection at the public Query boundary. The world facet is required, but a selection is a collection result rather than a required non-empty context identity: no selected actors therefore returns a successful complete snapshot with zero entries. It must not fail with `execution_context_required_facet_missing` or synthesize a placeholder identity.

For each single `content_graph` facet, the public success payload is the business
projection itself with the minimal shape `{"entries":[...]}`.
`content_graph.compound` instead returns `{"sections":{...}}`, grouping each
requested business category once. It never exposes a cursor, snapshot/revision,
owner, selector, command envelope, or any hash/GUID-like
machine identity. The internal typed snapshot keeps those fields for service
coordination and readback, but they are not part of the model-facing result.
Originally empty business objects and observed empty arrays remain present;
objects emptied solely by removal of private fields may be omitted. Shared
record projections do not truncate to the first owner or skip malformed rows.
A single-record result requires exactly one matching record. Invalid nested
values or missing required fields return structured diagnostics with the public
field/index path instead of a successful partial or empty result.
Non-content-graph Query routes retain their own
current contracts until separately migrated.

Distinct failures remain distinct. They include missing `uai:query` authority, cancellation, invalid parameters, a missing exact target, an unsupported command, invalid thread/context/state plane, descriptor/provider failure, projection failure, or a provider that cannot attest read-only behavior. None falls back to a prefix, alias, old route or another provider.

Explicit context and projection fields are validated before provider execution.
Unknown fields, invalid types and unsupported state planes return a structured
input diagnostic. An explicit empty `selection_identities` array clears selection;
omitting the field retains the current context. Projection reports a missing
required field in a recognized record instead of silently dropping that record.

Candidate catalog completeness is checked at its source, separately from result
transfer counts and display-name grouping. Known source-enumeration gaps return a structured
failure independently of keyword filtering. A known action's missing creation binding is
recorded only after that action matches the requested context/query, before display grouping;
unmatched actions and name-resolution-only context cannot invalidate the requested result.
The declared SoundCue, MetaSound and UMG partial catalogs retain their
existing capabilities with a public `diagnostics` warning. Description queries
can explain a specific action's support without creating it.

## Public SDK and CLI

`asset.sample` reads native mesh observations. `bounds` returns local-centimetre
origin/extents for StaticMesh or SkeletalMesh; `vertex` requires a StaticMesh LOD 0
vertex ID. `section` reads a StaticMesh LOD 0 render section (`index` defaults to
0), returning section/slot counts, triangle count, actual render material index
and path, and current/original editor section-map indices. An out-of-range
material reference is returned with `material_index_valid=false` and an empty
path so a missing material remains diagnosable. An invalid section index fails.
Only the requested asset's pending compilation is finished before reading render
data; the query neither edits slots nor opens an asset editor.

Diagnostics report coverage separately from messages and never trigger compile
or open an editor. Niagara stored VM freshness follows its native owner:
standalone Emitter definitions and non-compilable auxiliary scripts report
`not_applicable`, not proof of a clean compile. The successful native-context
compile result covers that operation; actual stored messages remain visible,
and System/compilable-script stale or pending state is not hidden.
Coverage sources include `owner/context/status`: `observed` means the source was
read, not that this operation compiled or executed it. PCG retained component logs
do not prove the current graph ran; MaterialFunction preview resources belong to
the existing preview context. Blueprint Dirty/Unknown remain stale/unknown.
Native update completion is operation-stage evidence, separate from diagnostics.

The final manifest projects these contracts into their ordinary domain
namespaces. Callers import only `uai`, use `uai.connect_project()` for the local project, and use
methods such as `client.project.summary(...)`,
`client.asset.relations(target, {"direction":"dependencies"})`,
`client.content_graph.items(...)`, and
`client.landscape.height_read(...)`. `client.execute(...)`,
`client.execute_exact(...)`, and `ueagentinterfacecmd exec` share the same generated
catalog, Client, Protocol, and Transport. No T06-specific Python binding
module, command-ID table, contract-hash table, alias layer, transport, or
result interpreter remains authoritative.

```text
ueagentinterfacecmd exec project.summary --params-json '{"projection":{"profile_id":"compact"}}'
```

## Legacy exit and evidence boundary

The old read/export contract and handler registry no longer exist in the active compiled tree. The seven read-only `content_graph.*` facets now belong to `query-command-owner` and resolve through ContentGraphRead providers; the T13 workspace contribution retains only `content_graph.import_workspace` and `content_graph.export_workspace`. The sole immutable registry therefore has no duplicate content-graph read route. T44 also isolated the obsolete module-owned Query-service composition and its `ReferenceCommand` handler.

The immutable registry owns exact dispatch, typed failures, descriptor-derived schemas and
centralized lifecycle behavior without a command-count oracle. Manifest projection retains each
frozen field schema, and active `asset.relations(..., input)` forwards the nested object once.
Normal discovery and generated named bindings hide dormant identities; compatibility discovery
exposes them with their active successors, while exact dormant execution remains available through
the compatibility entry point.
