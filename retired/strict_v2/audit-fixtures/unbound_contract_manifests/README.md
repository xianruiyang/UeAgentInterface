# Unbound contract manifests

Status: retired audit evidence; not an active fixture root, contract source, test oracle, generator input, or runtime
owner.

## Preserved source paths

These five manifests were moved without changing their Git or raw blob content:

- `Plugins/UeAgentInterface/Tests/Fixtures/CatalogWorkspaceAssemblyContract/manifest.hjson`
- `Plugins/UeAgentInterface/Tests/Fixtures/ExportSafetyContract/manifest.hjson`
- `Plugins/UeAgentInterface/Tests/Fixtures/HjsonDialect/manifest.hjson`
- `Plugins/UeAgentInterface/Tests/Fixtures/HjsonEditNavigationDiff/manifest.hjson`
- `Plugins/UeAgentInterface/Tests/Fixtures/HjsonEditNavigationDiff/reference/workspace/manifest.hjson`

The directory structure below this README preserves the suffix after `Tests/Fixtures/`.

## Why they are isolated

- The manifests reference 43 companion `.hjson` files; none exists in the current project.
- Two references target the deleted `UeAgentInterfaceCMD/cli/uai_core` owner, which project policy forbids restoring.
- Their declared `UAI.CatalogWorkspace.*`, `UAI.ExportSafety.*`, `UAI.HjsonDialect.Python.AtomicWrite`, and
  `UAI.HjsonEdit.*` test identities have no active implementation in the bounded Plugin/CMD/project test search.
- No active literal or generic discovery consumer was found for the former Plugin `Tests/Fixtures` root. This negative
  result does not rule out an unregistered external manual consumer, so the minimal manifests are retained for audit
  instead of deleted.

The manifests' own `development_truth` fields are historical claims, not current authority. Do not recreate their
missing corpus, restore `uai_core`, or route tests here merely to satisfy those claims.

## Re-entry and recovery

Move any contract back into an active test root only when a current user-confirmed requirement, a named long-term owner,
a real consumer, a complete self-contained corpus, and a valid test/verification path have all been established. That
change must remove or replace stale references and verify the consumer through the normal project workflow.

For byte recovery or the former paths, use Plugin commit
`b19e2dba34875c8d5337876d4332a71b31a06959` and the registered `codex/recovery/root-env-20260819-plugin-*` refs.
The current root-plan evidence is `TROOT-ENV-14-PLUGIN-RESIDUAL-SEPARATION`; this README does not duplicate its task
state or completion decision.
