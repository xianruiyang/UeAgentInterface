# Read and semantic export command contract
## Public authority

Read, search, query, probe, validation, difference inspection, and semantic
export use the shared `POST /api/uai/command` adapter and
`uai.command.v1` wire schema. Their public identities and fields come only
from the process-active `FUeAgentImmutableCommandRegistry`; the committed
manifest and generated Python bindings are one-way projections.

Public parameters never expose descriptor IDs, provider bindings, route
selectors, HTTP paths, or another dispatch language. Internal descriptor/route
inventories may remain implementation details, but they neither define nor
supplement the public command set.

## Command and content discovery

Unknown command identities are discovered through
`query.command.search`, which returns canonical names only. Batch
`query.command.usage` returns each selected name's minimal named Python call;
`generated SDK 与正式 schema(command, variant?)` is used only when full schema or
failure semantics are needed. There is no public `reference.catalog_search`.

Object, asset, node, component, Actor, field and creation candidates belong to
the applicable domain Query surface. A selected readable business name or
short scoped alias must be accepted unchanged by the downstream public read or
authoring command. Machine identities are resolved by the domain owner or an
SDK-owned handle and are never copied by the model.

## Read behavior

Query and read handlers are side-effect free. They validate the public input,
resolve the exact domain owner, and return a typed observation with its
readable business state, completeness and applicable partial/failure
information. Machine identity, revision and evidence stay in the private SDK
envelope or server custody.
An internal result envelope being structurally valid is not business success;
callers check the command Result and its query/operation error before consuming
payload fields.

## Semantic export

The model-facing `content_graph.export_workspace` SDK method accepts:

- required `target`;
- optional short `name`;
- optional `profile_id`.

The SDK derives and privately carries the absolute `workspace_path`; the model
receives an `HjsonWorkspaceHandle` whose `.path` is short and project-relative.

The server performs a fresh content-graph read, composes the semantic authoring
model and export plan, and atomically publishes the workspace. Token reduction
comes from semantic projection, profiles, references and deterministic file layout.

The returned artifact/receipt is evidence only for the command's declared
scope. A successful transport envelope does not by itself prove a complete
workspace, correct target coverage, or later importability.

## Retired routes and second truths

The retired `/api/uai/reference` and `/api/uai/export` routes, route-local
writers, route-local serializers, Python CommandSpec registries, and historical
command catalogs are not active and are not fallback paths. They are not restored
to satisfy an obsolete command count or receipt.

## Evidence boundary

Discovery, reference read, and semantic export identities come from the immutable
manifest. `reference.catalog_search` is absent, generated bindings consume those
same identities, and public fields expose no internal dispatch data.
Domain product readiness still requires current public execution, real UE
observation, applicable persistence/readback, failure and cleanup evidence.
