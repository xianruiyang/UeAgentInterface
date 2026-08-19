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

Query and Discovery facets are fail-closed until their production projections
are connected. A declaration cannot silently publish one of those facets through
an old registry or a consumer-local fallback.

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
process authority, exact command/HJSON projection counts, identical authority
pointers through the legacy-named accessors, duplicate and sealing failures,
absence of omitted-contribution refill, exact final-registry manifest projection,
and cleanup of the owned worker. They prove the activation and projection seam.
They do not prove every command's UE mechanics, persistence, readback, public
transport, or domain completeness; each domain still requires its own public and
product-level evidence.
