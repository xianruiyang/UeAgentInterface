# T09 Unified HJSON Import Planning Core

## Scope

T09 establishes the side-effect-free planning and provider-resolution boundary between the unified HJSON semantic model and later domain execution. It proves common parsing output, explicit change semantics, prewrite validation, initialization-graph planning, unique domain-provider registration and resolution, resource failure, and cleanup behavior. It does not claim a public domain import or any real UE object creation; T13 and the domain tasks consume this plan through their registered native creation providers.

The only import-planning direction is:

`directory | single_file | inline -> TryLoadHjsonImportSource -> unified semantic model -> explicit change set -> preflight -> initialization graph -> exact provider -> exact impact authorization -> immutable plan`

`TryLoadHjsonImportSource` is the common side-effect-free source boundary. `directory` fixes one normalized root, rejects root/file/ancestor reparse points, reads only the manifest-declared HJSON set, rejects undeclared files and unsafe relative paths, validates schema versions and content hashes, and rejects a source whose file set, size, or timestamp changes while it is being resolved. `single_file` and `inline` validate the same embedded manifest and units through the same resolver. Multi-file content exists only inside a directory workspace; it is not a fourth public source variant or a separate business route.

Once the three public carriers resolve to the same `FUnifiedHjsonSemanticModel`, they produce the same planning semantics. Full-state, minimal-create, and local-edit intent remains on that common model. Omitted fields, items, relations, order, and layout do not synthesize deletion; create, delete, replace, clear, reorder, connect, disconnect, set, null, restore-default, and remove are explicit change kinds.

## Prewrite contract

`TryPlanHjsonImportInitialization` validates the source baseline and manifest fingerprints, exact target revision, every explicit change, candidate binding revision, initialization node and edge, impact scope, domain support, and authorization before publishing a plan. Candidate names are passed to the preflight owner unchanged; an implicit alias or remap fails with `hjson_import_candidate_identity_changed`.

The planner owns no UE mutation, save, reload, rollback, or client-side name mapping. A failure resets the output plan and never calls authorization when an earlier resource, version, candidate, support, or graph check has failed. The authorization decision is bound to the stable plan fingerprint and sorted exact impact scopes.

## Initialization graph

Every declared node binds a stable local identity, role, type identity, and one initialization-contract identity. Edges are one of:

- ownership: one lifecycle owner per child and no cycles;
- creation dependency: deterministic ordering before identity creation;
- reference: deferred until identities exist, with shared references and cycles permitted.

Ownership and creation dependencies are ordered with an explicit iterative worklist. There is no artificial nesting-depth limit and no recursive traversal dependency on the C++ call stack. Duplicate nodes, duplicate edges, missing endpoints, multiple owners, ownership cycles, and creation-dependency cycles fail before plan publication. Reference edges remain in the immutable plan for the later binding phase.

## Domain provider registry

`FImportInitializationDomainProviderRegistry` freezes an explicit provider set before execution. Provider identity must be exact and unique. A request carries the public command, schema, target, root type, structural role, initialization-contract identity, and canonical parameters; resolution requires one and only one provider to match the request and support that exact role/contract pair.

No match returns `hjson_domain_provider_unresolved`, duplicate provider identity fails freeze with `hjson_domain_provider_duplicate`, and more than one runtime match returns `hjson_domain_provider_ambiguous`. The process factory catalog seals on first freeze and rejects late registration. Only the resolved `IImportInitializationDomainProvider` can enter the domain execution port; handlers and domain adapters may not add fallback type branches or copy common graph traversal.

## Resource and failure semantics

The planner charges the shared node, item, reference-edge, work, and memory budgets for the whole request. Exhaustion returns the shared `resource_budget_exceeded` error with stopped-early evidence; it never truncates a graph or publishes a partial plan.

Other stable failures cover invalid request/change/node/edge/impact, target revision read or conflict, unsupported changes, invalid or changed candidates, ownership/dependency cycles, and failed exact-impact authorization. Provider-owned errors remain intact when supplied. Successful output records the source model fingerprint, exact target revision, explicit changes, deterministic creation order, deferred references, resolved exact candidates, impact scopes, and plan fingerprint.
