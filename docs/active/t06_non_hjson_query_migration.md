# T06 non-HJSON Query migration

## Active ownership boundary

The `query` process contribution publishes exactly forty non-HJSON read commands through `query-command-owner`. They join the one process-level immutable registry and exact dispatch owned by the active command composition; the contribution does not create a private registry or a second public route.

The inventory is:

- asset state: `asset.collection`, `asset.properties`, `asset.registry_record`, `asset.registry_relations`, `asset.relations`, `asset.sample`, `asset.summary`, `asset.validation`;
- configuration and Editor state: `config.resolved_value`, `config.section`, `editor.dirty_resources`, `editor.open_assets`;
- project state: `project.platform`, `project.settings`, `project.summary`, `project.validation`;
- reference state: `reference.asset_read`;
- world state and runtime probes: `world.actor_components`, `world.actors`, `world.capsule_path`, `world.capsule_sweep`, `world.content`, `world.data_layers`, `world.hlod_assignments`, `world.mesh_closest_vertex`, `world.mesh_vertex_position`, `world.nav_connectivity`, `world.nav_find_path`, `world.nav_project_point`, `world.nearby_shapes`, `world.overlap`, `world.partition`, `world.partition_health`, `world.physics_component`, `world.physics_constraint`, `world.property`, `world.ray_trace`, `world.selection`, `world.streaming_levels`, `world.summary`.

Thirty-nine public IDs select the descriptor with the same mechanics ID. `reference.asset_read` is a separate exact public contract whose server-owned route selects `asset.properties` mechanics and freezes `BlueprintDisplayName`, the persisted property written by `reference.asset_apply`. The SDK does not translate it into `asset.properties`, and callers cannot substitute one identity for the other.

## Request, mechanics and result

Every request enters `/api/uai/command`, passes the exact command contract, and is dispatched once to the Query handler. The handler requires `uai:query`, preserves the exact command ID, canonicalizes the supplied parameter object, and delegates once to `IUeAgentQueryService`.

Commands that address one object require `target`; `reference.asset_read` instead requires `object_path` and the server supplies the exact `BlueprintDisplayName` property selection. Optional `input` contains only descriptor-specific query fields, `context` selects an allowed current state plane and execution context, and `projection` controls fields, provenance and bounded paging. The service does not use HJSON and does not perform alias guessing.

The production service composes the audited Asset/Project and World descriptor registries, one provider for each exact mechanics binding, real native UE read hooks, current Editor/World context, a bounded resource tracker, and the shared response projector. Real mechanics execute on the game thread. They are read-only and do not save or mutate assets. Projection may page or report partial/degraded source state; it is not rejected solely because an estimated total token count is large.

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
`client.reference.asset_read(...)`. `client.execute(...)`,
`client.execute_exact(...)`, and `uai-cli exec` share the same generated
catalog, Client, Protocol, and Transport. No T06-specific Python binding
module, command-ID table, contract-hash table, alias layer, transport, or
result interpreter remains authoritative.

```text
uai-cli --connection connection.toml exec project.summary --params-json {"projection":{"max_items":25}}
```

## Legacy exit and evidence boundary

The old read/export contract and handler registry no longer publish any route. T13 moved the former `content_graph.*` routes into its active contribution, so neither those routes nor any of the forty T06 IDs remains in the legacy registry. The reference command handler factory no longer publishes `reference.asset_read`; its dormant `reference.asset_apply` implementation is not registered by the process active composition and is retained only as internal mechanics pending T90 physical retirement. `reference.asset_read` therefore has one active public owner and composes the shared asset mechanics only behind the new Query service.

T06 Automation verifies the exact forty-command inventory, unique owner, legacy-route exit, exact dispatch, typed failures and a real production `project.summary` readback. The public SDK evidence additionally discovers all forty current contract hashes through `query.command.search`, executes `client.project.summary` through the live Editor, and uses `client.reference.asset_read` to read the exact persisted `BlueprintDisplayName` of a targeted reference Blueprint. It also observes `query_target_required` for a targetless `asset.summary`. These claims do not cover HJSON authoring, mutation, persistence or any Query command outside the inventory above.

The authoritative historical mapping is the adjacent
`successors/t06_non_hjson_query_migration.json` v2 record. It consumes all 21
active frozen obligations assigned to T06, preserves their purpose/input/output
contracts, names one exact current Query command for each row, and records the
absence of every old public ID, manifest entry, SDK binding, and compatibility
alias. That record is validated before a T06 verification receipt can be
emitted; T61 only aggregates it and does not infer or implement a successor.
