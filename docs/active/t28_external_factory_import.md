# T28 Shared Factory External Import

## Scope

The active `external-import-factory` contribution owns exactly eleven public routes: seven create imports (`FBX animation`, `FBX skeletal mesh`, `geometry cache`, `light profile`, `sound wave`, `sparse volume texture`, and `texture`) plus four matching asset reimports (`sound wave`, `sparse volume texture`, `static mesh`, and `texture`). Its public owner is `factory-external-import-owner`.

`source_ref`, `destination_ref` or `target_ref`, the FBX animation `skeleton_ref`, and `options` cross the public ingress unchanged. The handler performs only authority, game-thread, cancellation, and envelope adaptation. `OperationService` owns planning, impact authorization, version fences, native mutation, settle, persistence, independent verification, rollback, and terminal status. `FactoryExternalImportProvider` owns only the format-specific UE factory or reimport mechanics.

## Lifecycle and readback

Create routes require an absent `/Game` destination and a source basename equal to the destination asset name. Reimport routes require an existing class-compatible, non-dirty package and create a recoverable package backup before mutation. The source extension is validated per route before write.

Success is returned only after the formal UE factory or reimport handler completes, real `AssetImportData` contains the exact source, the primary asset and every generated dependency package save, all owned packages unload and reload, and persisted source hash, options fingerprint, operation, class, ImportData class, business state, and dependency identities read back equivalently. The FBX skeletal route owns and persists its generated `USkeleton`; the FBX animation route must resolve the caller-supplied skeleton. Geometry cache creation uses `UAlembicImportFactory` in Geometry Cache mode and sparse-volume creation uses `USparseVolumeTextureFactory`, so neither route relies on the generic automated-factory selection order.

Business-state verification rejects factory objects that exist but are not usable: animation requires sampled keys and the exact skeleton; skeletal mesh requires bones and a skeleton; geometry cache requires tracks; light profile and texture require initialized texture source data; sound wave requires positive duration; sparse volume texture requires frames and non-zero volume resolution. The independent readback exposes `business_state_kind`, `business_item_count`, and `dependency_identities` together with the persistence receipt. A prewrite failure is `rejected`; a confirmed post-mutation cleanup is `rolled_back`; an unconfirmed rollback is `state_unknown` and is never reported as success.

Supported common options are `replace_existing`, `import_materials`, `import_textures`, and `create_physics_asset`. Texture import additionally accepts `srgb`, `compression_settings`, `mip_gen_settings`, and `lod_group`; all four are applied to the imported `UTexture2D`, saved, unloaded/reloaded, and returned by independent readback. Skeletal FBX import additionally accepts `skeleton_ref`, `import_animations`, `import_morph_targets`, `update_skeleton_reference_pose`, `validate_after_import`, `expected_morph_targets[]`, `expected_material_slots[]`, and `expected_lod_count`; expected values are checked against the persisted mesh after reload. Format-specific options are rejected on other routes. `replace_existing=false` is required for create and `true` for reimport. Unknown options fail before mutation.

## Public SDK and CLI

The final manifest projects all create imports into the default `external`
namespace and all reimports into `asset`. Callers import only `uai`, connect
from one TOML, and use methods such as
`client.external.fbx_animation_import(...)`,
`client.external.sound_wave_import(...)`, and
`client.asset.reimport_texture(...)`. Generic `client.execute(...)`, exact
`client.execute_exact(...)`, and `uai-cli exec` reuse the same generated
catalog, Client, Protocol, and Transport. No T28-specific Python module,
factory selector, option mapper, command/hash table, retry loop, persistence
workflow, or readback implementation remains authoritative.

```text
uai-cli --connection connection.toml exec external.texture.import --params-json {"source_ref":"D:/fixtures/T_Grid.png","destination_ref":"/Game/StrictV2/T28/T_Grid","options":{"replace_existing":false}}
```

## Legacy exit

The module activates these commands only through the process-wide exact contribution and does not expose the historical public routes from `ExternalDataImport` or `AssetDestructiveImport`. T44 removed the compiled legacy registry/HTTP handler; any retained implementation mechanics are private dependencies of current owners and cannot act as fallback owners for a T28 command.

The active historical identities `asset_import_fbx_animation`, `asset_import_fbx_skeletal_mesh`, and `asset_import_texture` are non-executable. Their exact successor and field-by-field semantic decisions are recorded in `successors/t28_external_factory_import.json`; none is present in the active manifest, SDK catalog, or compatibility alias table.
