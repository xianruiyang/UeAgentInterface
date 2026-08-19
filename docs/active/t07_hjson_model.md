# T07 Unified HJSON Model Contract

## Scope

T07 establishes the common HJSON semantic model used after canonical state capture and before export, import, or domain mechanics. It is a module contract only: it does not claim that any asset domain or public command has completed migration.

The only semantic direction is:

`real UE state -> canonical snapshot -> FAuthoringModel -> FUnifiedHjsonSemanticModel -> FUnifiedHjsonWorkspace / FUnifiedHjsonSelectionPackage`

Physical directory, single-file, and inline carriers are projections of the same semantic fingerprint. Token estimates are telemetry for projection choices and are never a completeness gate.

## Common composition schema

Every unified model carries the same twelve named composition slots: target, property set, member set, structure, item, endpoint, relation, order, layout, temporal contract, spatial contract, and external input. A slot may contain multiple units or be empty, but all source semantic units must have exactly one composition owner. Full-state, minimal-create, and local-edit are intents over this same model; they do not select separate parsers or semantic schemas.

`TryCreateUnifiedHjsonSemanticModel` rejects missing slots, duplicate or unbound units, unknown units, incompatible unit kinds, and target bindings that do not match the core model. Its model fingerprint is independent of authoring intent.

## Field authority and intent

`FExportFieldRegistry` remains the single field-owner authority. `FUnifiedHjsonFieldRegistry` adds authoring intent without creating a second field registry. Every exported field has exactly one unified contract.

Omission always resolves to `preserve`. Mutation requires one explicit intent: `set_value`, `set_null`, `restore_default`, or `remove`. Null is allowed only for a declared nullable field; restore-default requires a reproducible default; remove requires presence-sensitive semantics. Unsupported combinations fail before mutation with a typed `hjson_field_*` error.

## Workspace index and selection

`FUnifiedHjsonWorkspaceIndex` records every unit, its composition, owner, field-set owner, and direct relations. `TryCreateUnifiedHjsonWorkspace` accepts a layout only when it carries the same core semantic fingerprint.

`TryResolveUnifiedHjsonSelection` delegates owner, controlling-field, placement, and endpoint closure to the existing shared selection owner. It adds direct relation IDs from the resolved semantic units and then reuses that same owner for the final closure; it does not implement a parallel selection algorithm.

## Depth and resource failures

HJSON parsing and unified-model construction use iterative or bounded-resource processing. There is no fixed artificial nesting limit. Each request supplies explicit multi-dimensional resource budgets. Exceeding a budget returns the existing typed resource error with observed usage and `stopped_early=true`; the system does not silently truncate or reinterpret the document.
