# T06 non-HJSON Query migration

## Active ownership boundary

The `query` active-command contribution publishes exactly forty-two non-HJSON read routes through `query-command-owner`. They join the one process-level immutable command registry and exact dispatch owned by the active command composition; the contribution does not create a private command registry or a second public route. Provider authority is separately frozen by `DomainCapability`: the Query domain contributes forty-one established providers and the Landscape domain contributes `landscape.read.height`.

The inventory is:

- asset state: `asset.collection`, `asset.properties`, `asset.registry_record`, `asset.registry_relations`, `asset.relations`, `asset.sample`, `asset.summary`, `asset.validation`;
- configuration and Editor state: `config.resolved_value`, `config.section`, `editor.dirty_resources`, `editor.open_assets`;
- Landscape state: `landscape.height.read`;
- project state: `project.platform`, `project.settings`, `project.summary`, `project.validation`;
- reference state: `reference.asset_read`, `reference.origin.resolve`;
- world state and runtime probes: `world.actor_components`, `world.actors`, `world.capsule_path`, `world.capsule_sweep`, `world.content`, `world.data_layers`, `world.hlod_assignments`, `world.mesh_closest_vertex`, `world.mesh_vertex_position`, `world.nav_connectivity`, `world.nav_find_path`, `world.nav_project_point`, `world.nearby_shapes`, `world.overlap`, `world.partition`, `world.partition_health`, `world.physics_component`, `world.physics_constraint`, `world.property`, `world.ray_trace`, `world.selection`, `world.streaming_levels`, `world.summary`.

Forty-one public IDs select a descriptor with the same mechanics ID. `reference.asset_read` is the separate exact public contract whose server-owned route selects the internal `reference.read` descriptor and freezes `BlueprintDisplayName`, the persisted property written by `reference.asset_apply`. Its dedicated AssetProjectRead provider is the only read path on which a missing asset is a successful complete persisted snapshot with zero entries; generic `asset.properties` continues to reject an unresolved target. The SDK does not translate either command into the other, and callers cannot substitute one identity for the other.

## Request, mechanics and result

Every request enters `/api/uai/command`, passes the exact command contract, and is dispatched once to the Query handler. The handler requires `uai:query`, preserves the exact command ID, canonicalizes the supplied parameter object, and delegates once to `IUeAgentQueryService`.

Commands that address one object require `target`; `reference.asset_read` instead requires `object_path` and the server supplies the exact `BlueprintDisplayName` property selection. Optional `input` contains only descriptor-specific query fields, `context` selects an allowed current state plane and execution context, and `projection` controls fields, provenance and bounded paging. The service does not use HJSON and does not perform alias guessing.

The production service composes the audited Asset/Project, World, and Landscape descriptor registries. It resolves every provider from the cached frozen process `DomainCapability` authority at execution time; it does not construct or retain a local provider registry. Real native hooks execute on the game thread with the current Editor/World context, a bounded resource tracker, and the shared response projector. They are read-only and do not save or mutate assets. Projection may page or report partial/degraded source state; it is not rejected solely because an estimated total token count is large.

The process composition binds World reads to the current Editor World. If the
Editor changes maps, the process host invalidates the cached World runtime and
rebuilds the World-backed handlers before resolving the next command; a
handler composed for the previous map is not reused against the new map.

`editor.dirty_resources` owns a read-only dirty-resource observation rather
than reusing the open-asset set. For `map` resources it observes loaded World
packages, their loaded map-build-data packages, and loaded External Actor
packages. For `asset` resources it observes ordinary dirty content packages,
and configuration kinds remain separate. `resource_kinds` filters these
sources before projection. The query does not call an Editor save helper whose
enumeration would itself mark a newly created World dirty.

`world.nav_find_path` and `world.nav_connectivity` both expose
`allow_projection_failure`. It is meaningful only when `project_to_nav` is
true; supplying it, or a projection extent, while projection is disabled
fails with `world_read_navigation_projection_policy_invalid` instead of being
silently ignored.

`world.selection` freezes the current actor selection at the public Query boundary. The world facet is required, but a selection is a collection result rather than a required non-empty context identity: no selected actors therefore returns a successful complete snapshot with zero entries. It must not fail with `execution_context_required_facet_missing` or synthesize a placeholder identity.

A successful response contains:

- `command_id`: the unchanged public identity;
- `mechanics_command_id`: the exact server-side mechanics identity;
- `read_only: true`;
- `query_result`: the typed projected result, including its descriptor/source identity, state semantics, revision and completeness where supplied by the provider.

Distinct failures remain distinct. They include missing `uai:query` authority, cancellation, invalid parameters, a missing exact target, an unsupported command, invalid thread/context/state plane, descriptor/provider failure, projection failure, or a provider that cannot attest read-only behavior. None falls back to a prefix, alias, old route or another provider.

## Public SDK and CLI

The final manifest projects these contracts into their ordinary domain
namespaces. Callers import only `uai`, connect from one TOML file, and use
methods such as `client.project.summary(...)`,
`client.world.actor_components(...)`, and
`client.reference.asset_read(...)`, and
`client.landscape.height_read(...)`. `client.execute(...)`,
`client.execute_exact(...)`, and `uai-cli exec` share the same generated
catalog, Client, Protocol, and Transport. No T06-specific Python binding
module, command-ID table, contract-hash table, alias layer, transport, or
result interpreter remains authoritative.

```text
uai-cli --connection connection.toml exec project.summary --params-json {"projection":{"max_items":25}}
```

## Legacy exit and evidence boundary

The old read/export contract and handler registry no longer exist in the active compiled tree. T13 moved the former `content_graph.*` routes into its current contribution, so neither those routes nor any of the current forty-two Query IDs is duplicated in the sole immutable registry. T44 also isolated the obsolete module-owned Query-service composition and its `ReferenceCommand` handler. `reference.asset_read` therefore has one active public owner and composes shared asset mechanics only behind the current Query service.

T06 Automation verifies the exact forty-two-route process inventory, unique owner, legacy-route exit, exact dispatch, typed failures, a real production `project.summary` readback, and the complete zero-entry result of an empty current `world.selection`. Product evidence additionally executes `client.landscape.height_read` through the live Editor against an exact managed Landscape before and after native mutation, verifies bounded local heights and no package dirtiness, and confirms exact absence after cleanup. These claims do not cover HJSON authoring or any Query command outside the inventory above.

The authoritative historical mapping is the adjacent
`successors/t06_non_hjson_query_migration.json` v2 record. It consumes all 21
active frozen obligations assigned to T06, preserves their purpose/input/output
contracts, names one exact current Query command for each row, and records the
absence of every old public ID, manifest entry, SDK binding, and compatibility
alias. That record is validated before a T06 verification receipt can be
emitted; T61 only aggregates it and does not infer or implement a successor.
