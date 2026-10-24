# T35 Active command composition

## Process ownership boundary

`DomainCapability` owns the only process-scoped capability catalog and the only
published capability-authority snapshot. A domain registration translation unit
may contribute a complete `FDomainCapabilityContribution`, a command facet, an
HJSON facet, or a combination of those facets under one exact domain identity.
The catalog assembles those registrations into one declaration per domain before
anything is projected.

`FUeAgentActiveCommandComposition` is a pure command projector. It accepts an
exact array of command contributions and produces one immutable registry and one
exact dispatch; it owns no process catalog, performs no fallback lookup, and
cannot refill a contribution omitted by its caller. The HJSON provider registry
uses the same rule: it freezes only the exact provider array supplied by the
frozen domain declarations and owns no parallel process catalog or installed
authority.

The current Query command contribution owns `query.command.search` and
`query.command.usage`. Public parameter-schema inspection uses the SDK/CLI's local
`describe` projection from the generated binding.
Command discovery is constructed from the final immutable command registry, so
it observes the complete registered set and applies the centralized lifecycle
projection without aliases or client-side mappings; it is not a second registry
owner.

The central command-lifecycle policy is the only owner of dormant compatibility
classification. It is applied before the registry freezes: ordinary discovery and
named generated SDK methods expose only active commands, while the catalog retains
dormant identities for explicit compatibility inspection and exact dispatch. Domain
registrations, handlers, search and SDK generation do not maintain per-command
dormancy branches.

The `unified-content-search` contribution owns the only current
`query.content.search` command and contributes both current providers as one
atomic domain capability. `editor.asset_registry.content` searches persisted
Asset Registry identities. `editor.world.content` searches Actor and Component
identities in the current Editor World. The shared content-discovery service
owns provider selection, internal freshness and provider failure isolation;
handlers and clients do not recreate those rules. Public discovery returns only
readable names and asks for refinement instead of exposing paging machinery.

This unified command searches existing content only. Creation candidates remain
with the twelve exact domain candidate commands. Each returns readable
base names grouped for display, with one shared query `scope`. Authoring uses `name` plus at most
two returned short semantic `select` fields and keeps graph, pin, parent, level,
stage, placement, and instance identity in the owning HJSON context. There is
no current `node_catalog_search`, `node_catalog_categories`, universal rich
candidate row, or client-side candidate mapping.

## Atomic capability activation

Process activation performs one bounded sequence:

1. Seal the DomainCapability process catalog and copy its domain entries in
   stable domain-id order.
2. Instantiate each complete declaration and each registered facet; reject
   invalid, duplicate, missing, or mismatched domain, contribution, provider,
   command, route, operation, or other declared identities.
3. Freeze one immutable domain-capability set.
4. Project the exact command contributions, exact HJSON providers, and target
   composition contracts from that frozen set. Command composition freezes the
   immutable command registry once, constructs handlers against that final
   registry, and creates exact dispatch once.
5. Cache and publish the complete `FDomainCapabilityAuthorities` snapshot only
   after every connected projection succeeds.

No command registry, dispatch, HJSON provider registry, or cached process
authority is published when any projection fails. Late process registration is
rejected after sealing. Repeated server starts in the same module process reuse
the same frozen capability set and authority snapshot; they do not reopen
registration or recompose an independently mutable command/HJSON catalog.

The current frozen authority projects the Query provider registry, generic
Discovery service, and content-discovery service together with command and HJSON
authorities. Invalid, duplicate, missing, or disconnected facets fail before the
authority snapshot is published. A declaration cannot silently publish one of
those facets through an old registry or a consumer-local fallback.

## Public identity contract

`query.content.search` accepts an explicit `content_kinds` subset of `asset`,
`actor`, and `component`, plus readable matching terms. Results
contain only readable names, using the shortest semantic alias needed for
ambiguity. They do not expose provider identity, revision, object identity,
class/path metadata, paging, detail requests, or operation metadata.

An Asset, Actor, or Component result is only a discovery name. The owning
command re-enumerates its current context when that name is supplied; zero or
multiple matches fail closed. Search returns all matches without a result-count gate.

## Candidate query scope

Context-dependent candidate queries use the shared name projection and attach
one response-level `scope` through `TryAddDiscoveryScope`. It records the public
query context below; omitted optional selectors use the query's native defaults.
The matching text is a filter, not a creation context. No query history, opaque
identity or extra creation token is introduced.

| Query family | Scope fields |
| --- | --- |
| Blueprint node | target, graph_name; pin aliases remain in sibling candidate_context |
| Blueprint component | target, optional parent |
| Material expression | target, optional graph_name/pin_name |
| Niagara module | target, target_usage, optional emitter_name/stage_name |
| AnimBlueprint creation | future target, surface, supplied class_settings |
| PCG, audio, animation rig, UMG widget | target |
| AI behavior | target, supplied parent/attachment_kind or graph_owner |
| World actor | target, supplied category |
| Asset factory | create_command |

Names are reusable within the same context. A new target or menu context needs
discovery there; existing names are not globally valid creation selectors.
`scope` is not copied into HJSON. `describe` responses preserve the same scope.
Public candidate names, selectors and native re-resolution keep their existing
semantics; this metadata does not authorize reuse across contexts.

Candidate selection failures use the existing authoring diagnostic carrier to
preserve the error code, requested name and available `choose` fields, and identify
the current target/context and matching query. HJSON import adds the source file
and position through its existing source mapping; no separate diagnostic format
or candidate validation gate is introduced.

## Formal projection

`FUeAgentActiveCommandManifest` is the only formal command-manifest projector.
It consumes the command registry from the cached DomainCapability authority,
emits that exact command set and each frozen contract hash, and never reads the
legacy 329-command registry. Before the command registry is frozen, the pure
command projector assigns deterministic Python bindings from each command id:
the first segment is the namespace and the remaining segments, joined with
underscores, are the method.

The committed `Resources/UaiCommandManifest.json` is rebuilt only by the explicit
command-surface refresh action. Normal module startup obtains the single
DomainCapability authority and publishes Query ingress from that runtime
registry. A missing or stale manifest does not block startup, exact dispatch, or
other development work; the legacy registry and Python code are not fallback
runtime command sources.

## Extension contract

A domain with multiple facets should register one complete
`FDomainCapabilityContribution`. A command-only registration may use
`FUeAgentActiveCommandContributionFactoryRegistration`; an HJSON-only
registration may use
`FImportInitializationDomainProviderFactoryRegistration`. Both registrations
must carry the exact domain id when they belong to the same domain. Their
constructors delegate registration to DomainCapability and must not retain their
own process state.

Every command contribution supplies one stable unique contribution id, complete
command declarations through `CollectDeclarations`, and handlers through
`CreateHandlers`. Handlers receive the final aggregate registry when discovery
or binding depends on other commands. Future domains must not add a module-level
branch, a private process catalog, an override/fill-in path, or another installed
authority.

## Evidence boundary

The current DomainCapability and T35 runtime contracts describe one frozen
process authority, exact command/HJSON/Query/Discovery projection, provider
failure isolation, duplicate and sealing behavior, names-only command/content
discovery, and compact batch usage. Domain vertical evidence separately covers
the candidate owners, readable re-resolution and fail-closed ambiguity behavior.
