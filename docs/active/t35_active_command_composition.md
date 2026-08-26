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

The current Query calibration contribution remains the owner of
`query.command.search`, `query.command.detail`, and `query.calibration.read`.
Command discovery is constructed from the final immutable command registry, so
it observes the complete active command set without aliases or client-side
mappings; it is not a second registry owner.

The `unified-content-search` contribution owns the only current
`query.content.search` command and contributes both current providers as one
atomic domain capability. `editor.asset_registry.content` searches persisted
Asset Registry identities. `editor.world.content` searches Actor and Component
identities in the current Editor World. The shared content-discovery service
owns provider selection, request fingerprints, per-provider pagination,
revision-fenced cursors, completeness, and provider failure isolation; handlers
and clients do not recreate those rules.

This unified command searches existing content only. Creation candidates remain
with the eight exact domain commands `ai.behavior.candidates`,
`animation_rig_graph.candidates`, `audio.graph.candidates`,
`blueprint.component.candidates`, `blueprint.node.candidates`,
`material.expression.candidates`, `niagara.module.candidates`, and
`pcg.node.candidates`. Read each exact command detail before authoring. Domains
already on the contextual-create contract accept only the compact candidate
`name` or `id` as the minimal selector and keep graph, pin, parent, level,
stage, placement, and instance identity in the owning HJSON context. A legacy
field is valid only while that command detail still declares it. There is no
current `node_catalog_search`, `node_catalog_categories`, universal candidate
shape, or client-side candidate mapping.

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
`actor`, and `component`; exact/prefix/text/fuzzy matching; exact class, path,
owner, and required-operation filters; a per-provider limit; and an opaque
cursor copied from the previous response. Results retain provider identity,
revision, completeness, exact object identity, content kind, class, owner,
state layer, supported operations, and the matching detail command.

An Asset result identity is directly accepted by `asset.summary` and by an
applicable `content_graph.*` target. An Actor result identity is directly
accepted by `world.actor_components`; a Component result identity is directly
accepted by `world.property`. Clients must not trim, rebuild, alias, or convert
these identities. Reusing a cursor with changed semantic filters fails with
`content_search_cursor_request_mismatch`; changing provider state invalidates it
with `content_search_revision_stale`; selecting a kind with no provider fails
with `content_search_kind_unsupported`.

## Formal projection and startup gate

`FUeAgentActiveCommandManifest` is the only formal command-manifest projector.
It consumes the command registry from the cached DomainCapability authority,
emits that exact command set and each frozen contract hash, and never reads the
legacy 329-command registry. Before the command registry is frozen, the pure
command projector assigns deterministic Python bindings from each command id:
the first segment is the namespace and the remaining segments, joined with
underscores, are the method.

The committed `Resources/UaiCommandManifest.json` is generated only through the
explicit development test mode `UAI_UPDATE_COMMAND_MANIFEST=1`. Normal module
startup obtains the single DomainCapability authority and verifies the committed
manifest byte-for-byte before constructing Query ingress or publishing routes.
A missing or stale projection fails with
`active_command_manifest_file_missing` or
`active_command_manifest_projection_mismatch`; startup does not fall back to a
legacy registry or expose a partial command surface.

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

The current DomainCapability and T35 Automation contracts verify one frozen
process authority, exact command/HJSON/Query/Discovery projection, content
provider identity and paging rules, provider failure isolation, duplicate and
sealing failures, exact final-registry manifest projection, and cleanup of the
owned worker. The T35 public-SDK product run verifies Asset/Actor/Component
identity reuse, Material candidate-to-HJSON binding, paging, stale/mismatched
cursors, unsupported kinds, persisted readback, fixture cleanup, hidden Editor
startup, and safe exit. These results do not prove every query command, every
domain candidate, or future content provider; later domain verticals retain
those obligations.
