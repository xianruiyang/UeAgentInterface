# T28 Shared Factory External Import

## Scope

The active `external-import-factory` contribution owns exactly eleven public routes: seven create imports (`FBX animation`, `FBX skeletal mesh`, `geometry cache`, `light profile`, `sound wave`, `sparse volume texture`, and `texture`) plus four matching asset reimports (`sound wave`, `sparse volume texture`, `static mesh`, and `texture`). Its public owner is `factory-external-import-owner`.

The model supplies a short project-relative source or `ProjectFileHandle`, plus
`destination_ref` or `target_ref`, the FBX animation `skeleton_ref`, and
`options`. The SDK resolves the absolute source privately before the handler
performs authority, game-thread, cancellation, and envelope adaptation.
`OperationService` owns planning, impact authorization, version fences, native
mutation, settle, persistence, independent verification, rollback, and terminal
status. `FactoryExternalImportProvider` owns only the format-specific UE factory
or reimport mechanics.

## Lifecycle and readback

Both the asset reimport owner and Factory workspace reimport invoke the native
handler with automated mode enabled, forced dialogs disabled, and an unattended
script scope. Native confirmation dialogs must not suspend these API calls;
failures continue through the existing error and rollback paths.

Create routes require an absent `/Game` destination; its asset name may differ from the source filename. Reimport routes require an existing class-compatible, non-dirty package and create a recoverable package backup before mutation. Generic texture, sound and light-profile imports select a class-compatible native UE factory using its declared formats and import capability. Specialized FBX, geometry-cache and sparse-volume routes retain their native format requirements.

Success is returned only after the formal UE factory or reimport handler completes, real `AssetImportData` contains the exact source, the primary asset and every generated dependency package save, and the owned packages pass the internal settle/readback checks. The FBX skeletal route owns and persists its generated `USkeleton`; the FBX animation route must resolve the caller-supplied skeleton. Geometry cache creation uses `UAlembicImportFactory` in Geometry Cache mode and sparse-volume creation uses `USparseVolumeTextureFactory`, so neither route relies on the generic automated-factory selection order.

Business-state verification rejects factory objects that exist but are not usable: animation requires sampled keys and the exact skeleton; skeletal mesh requires bones and a skeleton; geometry cache requires tracks; light profile and texture require initialized texture source data; sound wave requires positive duration; sparse volume texture requires frames and non-zero volume resolution. The public success payload is intentionally short: `command_id`, `terminal_status`, and readable business `readback` containing target, asset class, state summary, relevant options, dependency references, and bounded counts. Source paths/hashes, option fingerprints, plan/receipt identifiers, lifecycle phase history, reload evidence, and full independent readback remain internal machine evidence and are not returned. A prewrite failure is `rejected`; a confirmed post-mutation cleanup is `rolled_back`; an unconfirmed rollback is `state_unknown` and is never reported as success.

Supported common options are `replace_existing`, `import_materials`, `import_textures`, and `create_physics_asset`. Texture import additionally accepts `srgb`, `compression_settings`, `mip_gen_settings`, and `lod_group`; the resulting option values may appear in readable `readback`, without exposing their fingerprint. Skeletal FBX import additionally accepts `skeleton_ref`, `import_animations`, `import_morph_targets`, `update_skeleton_reference_pose`, `validate_after_import`, `expected_morph_targets[]`, `expected_material_slots[]`, and `expected_lod_count`; expected values are checked internally against the persisted mesh. Format-specific options are rejected on other routes. `replace_existing=false` is required for create and `true` for reimport. Unknown options fail before mutation.

## Public SDK and CLI

The final manifest projects all create imports into the default `external`
namespace and all reimports into `asset`. Callers import only `uai`, use `uai.connect_project()` for the local project, and use methods such as
`client.external.fbx_animation_import(...)`,
`client.external.sound_wave_import(...)`, and
`client.asset.reimport_texture(...)`. Generic `client.execute(...)`, exact
`client.execute_exact(...)`, and `ueagentinterfacecmd exec` reuse the same generated
catalog, Client, Protocol, and Transport. No T28-specific Python module,
factory selector, option mapper, command/hash table, retry loop, persistence
workflow, or readback implementation remains authoritative.

```python
source = client.external_source.from_project("fixtures/T_Grid.png")
result = client.external.texture_import(
    source,
    "/Game/StrictV2/T28/T_Grid",
    options={"replace_existing": False},
)
```

Raw CLI absolute-path forms are machine integration surfaces and are not model guidance.

## Legacy exit

The module activates these commands only through the process-wide exact contribution and does not expose the historical public routes from `ExternalDataImport` or `AssetDestructiveImport`. T44 removed the compiled legacy registry/HTTP handler; any retained implementation mechanics are private dependencies of current owners and cannot act as fallback owners for a T28 command.
