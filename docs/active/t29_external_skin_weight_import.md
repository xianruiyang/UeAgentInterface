# T29 Skin Weight Profile Import

## Scope

The active `external-import-skin-weight` contribution owns exactly `external.skin_weight_profile.import` through `skin-weight-import-owner`. It accepts one existing persisted skeletal mesh, one profile name, one LOD index, and one external FBX source. The handler only adapts authority, game-thread execution, cancellation, and the public result envelope. `OperationService` owns fences, mutation, settle, persistence, independent readback, rollback, and terminal status. `SkinWeightImportProvider` alone calls the formal Skin Weight Profile importer.

## Lifecycle and readback

Preflight requires an existing non-dirty `/Game` skeletal mesh package, a valid LOD, an absent profile name, an existing FBX source, and a recoverable package backup. Success is returned only after the profile exists, source/profile/LOD provenance is written to profile-scoped package metadata, the package saves, unloads, reloads, and the exact profile plus provenance read back equivalently. A prewrite failure is `rejected`; a confirmed restoration is `rolled_back`; an unconfirmed restoration is `state_unknown` and is never success.

## Public SDK and CLI

The final manifest projects the command into the default `external` namespace.
Callers import only `uai`, connect from one complete Connection TOML, and call
`client.external.skin_weight_profile_import(source_ref, target_ref,
profile_name, lod_index)`. Generic `client.execute(...)`, exact
`client.execute_exact(...)`, and `uai-cli exec` resolve the same generated
catalog identity and contract hash. No T29-specific Python module, command/hash
table, retry loop, importer, persistence workflow, or readback implementation
remains authoritative.

```text
uai-cli --connection connection.toml exec external.skin_weight_profile.import --params-json {"source_ref":"D:/fixtures/Alternate.fbx","target_ref":"/Game/Characters/SK_Character.SK_Character","profile_name":"Alternate","lod_index":0}
```

Use `client.query.command_search("skin weight profile import", limit=5)` and
then `client.query.command_detail(command_id, contract_hash)` to obtain the
exact parameter schema, named SDK form, CLI form, owner, and tutorial
references before executing the selected exact identity.

## Legacy exit

The process composition activates this command only through the T29 contribution. Historical `ExternalDataImport` and destructive-import routes remain inactive migration inventory until `T90-LEGACY-ASSET-RETIREMENT`; they are not fallback owners.
