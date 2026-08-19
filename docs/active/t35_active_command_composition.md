# T35 Active command composition

## Active ownership boundary

`FUeAgentActiveCommandComposition` is the only owner that can publish the process-active immutable command registry and exact dispatch. Domain registration translation units contribute declarations and handler factories through stable contribution identities; they do not freeze a private registry, build a parallel dispatch, or require a central list of domains in `FUeAgentInterfaceModule`.

The currently active Query calibration contribution remains the owner of the three `query.command.search`, `query.command.detail`, and `query.calibration.read` contracts and handlers documented by T05. It is not the owner of the aggregate registry. Command discovery is constructed only after the aggregate registry is frozen, so it observes every active contribution without aliases or client-side mappings.

## Two-phase activation

Process activation performs one bounded sequence:

1. Seal the process contribution-factory catalog and copy its entries in stable contribution-id order.
2. Instantiate every contribution and reject missing or mismatched identities.
3. Collect every command declaration, then call `FUeAgentImmutableCommandRegistry::TryFreeze` exactly once.
4. Give that final registry to every contribution so it can construct its handlers and any registry-backed discovery service.
5. Call `FUeAgentExactCommandDispatch::TryCreate` exactly once and publish the registry and dispatch together.

No registry or dispatch is exposed when declaration validation, duplicate detection, contribution construction, handler construction, ownership validation, or dispatch construction fails. A failed local composition remains sealed. Duplicate contribution identities fail before composition; command-id and Python-binding duplicates fail during the single registry freeze. Any factory registered after the process catalog is sealed is retained as a typed activation error instead of entering a later partial registry.

Stopping and starting the UAI server in the same module process may recompose the same sealed factory snapshot. It does not reopen registration or mutate the previously defined contribution set.

## Formal projection and startup gate

`FUeAgentActiveCommandManifest` is the only formal manifest projector. It consumes the already frozen process registry, emits the exact command set and each frozen contract hash, and never reads the legacy 329-command registry. Before that one process-level freeze, the active composition assigns deterministic Python bindings from the command id: the first segment is the namespace and the remaining segments, joined with underscores, are the method. Registration-local task names therefore cannot leak into the public SDK, while the reusable immutable-registry primitive still validates explicitly supplied bindings and rejects duplicates.

The committed `Resources/UaiCommandManifest.json` is generated only through the explicit development test mode `UAI_UPDATE_COMMAND_MANIFEST=1`. Normal module startup recomposes the final registry and verifies the committed manifest byte-for-byte before constructing Query ingress or publishing any route. A missing or stale projection fails with `active_command_manifest_file_missing` or `active_command_manifest_projection_mismatch`; startup does not fall back to the legacy registry or expose a partial command surface.

## Extension contract

A domain contribution must implement `IUeAgentActiveCommandContribution` and provide:

- one stable, unique contribution id;
- all of its complete command declarations through `CollectDeclarations`;
- handlers through `CreateHandlers`, using the supplied final aggregate registry when discovery or binding depends on other commands.

The translation unit registers one `FUeAgentActiveCommandContributionFactoryRegistration`. Future command-migration tasks add their own registration unit and depend on this seam; they must not modify the module to append another domain branch or create another service-side command authority.

## Evidence boundary

The T35 Automation contract verifies aggregate registry visibility, cross-contribution discovery, duplicate contribution rejection, duplicate command rejection, late-registration rejection, sealing, failure without partial publication, exact final-registry manifest projection, and typed projection mismatch. It proves the composition and projection seam. Public transport behavior and the three current Query commands remain covered by the T05 public ingress and readback evidence; later domain tasks must obtain their own public receipts before claiming those commands are production-ready.
