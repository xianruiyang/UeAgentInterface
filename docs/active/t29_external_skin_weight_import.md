# T29 Skin Weight Profile Import

## Scope

The active `external-import-skin-weight` contribution owns exactly `external.skin_weight_profile.import` through `skin-weight-import-owner`. It accepts one existing persisted skeletal mesh, one profile name, one LOD index, and one external FBX source. The handler only adapts authority, game-thread execution, cancellation, and the public result envelope. `OperationService` owns fences, mutation, settle, persistence, independent readback, rollback, and terminal status. `SkinWeightImportProvider` alone calls the formal Skin Weight Profile importer.

## Lifecycle and readback

Preflight requires an existing non-dirty `/Game` skeletal mesh package, a valid LOD, an absent profile name, an existing FBX source, and a recoverable package backup. Success is returned only after the profile exists, source/profile/LOD provenance is written to profile-scoped package metadata, the package saves, unloads, reloads, and the exact profile plus provenance read back equivalently. A prewrite failure is `rejected`; a confirmed restoration is `rolled_back`; an unconfirmed restoration is `state_unknown` and is never success. The public success payload is intentionally short: terminal status plus target/profile/source/LOD business fields and the resulting profile count. Internal receipts, hashes, revision checks, lifecycle history, package paths, and full independent readback never enter the model-facing result.

## Public SDK and CLI

The final manifest projects the command into the default `external` namespace.
Callers import only `uai`, use `uai.connect_project()` for the local project, and call
`client.external.skin_weight_profile_import(project_relative_source, target_ref,
profile_name, lod_index)`. Generic `client.execute(...)`, exact
`client.execute_exact(...)`, and `ueagentinterfacecmd exec` resolve the same generated
catalog identity. No T29-specific Python module, command
table, retry loop, importer, persistence workflow, or readback implementation
remains authoritative.

```python
result = client.external.skin_weight_profile_import(
    "fixtures/Alternate.fbx",
    "/Game/Characters/SK_Character.SK_Character",
    "Alternate",
    0,
)
```

The SDK resolves the absolute source privately. Raw CLI absolute-path forms are
machine integration surfaces and are not model guidance.

For unknown command names or parameter shapes, follow [public command discovery](../../../../UeAgentInterfaceCMD/docs/PYTHON_SDK_AND_CLI.md#command-discovery).

## Legacy exit

The process composition activates this command only through the current external-import contribution. Historical `ExternalDataImport` and destructive-import public routes are absent from the immutable registry, and T44 removed the compiled legacy registry/HTTP handler. Retained private mechanics are not fallback owners.
