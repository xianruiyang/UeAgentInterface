# T30 Localization / StringTable

For unknown command names or parameter shapes, follow [public command discovery](../../../../UeAgentInterfaceCMD/docs/PYTHON_SDK_AND_CLI.md#command-discovery).
Discovery does not execute or replace any route listed below.

## Public routing

| Historical capability | Current command | Exact provider, capability, or policy |
|---|---|---|
| localization_apply_json | content_graph.import_workspace | semantic workspace for project LocalizationSettings target edits |
| string_table_apply_json | content_graph.import_workspace | semantic workspace for existing StringTable edits; separate initialization schema uai.string_table.edit/1 |
| localization_export_json / string_table_export_json | content_graph.export_workspace | LocalizationSettings / StringTable semantic composition |
| localization_diff_json / localization_validate_json | content_graph.compound | localization.diff / localization.validate |
| string_table_diff_json / string_table_validate_json | content_graph.compound | string_table.diff / string_table.validate |
| localization_query_targets / localization_report | content_graph.compound | localization.query_targets / localization.report |
| localization_preview_culture | editor.session plus operation lifecycle | policy:runtime-session:culture-preview/v1 |
| localization_gather_text | job.compile_build plus operation lifecycle | policy:owned-tool:localization-gather-text/v1 |
| localization_compile_text | job.compile_build plus operation lifecycle | policy:owned-tool:localization-compile-text/v1 |
| localization_export_po | job.compile_build plus operation lifecycle | policy:owned-tool:localization-export-po/v1 |
| localization_import_po | external.localization.po.import | localization-import-owner |

All compound capabilities are read-only. Mutation, commandlet execution, and file publication are
never hidden inside a Query.

## HJSON import and export

Model-facing directory edits use the `HjsonWorkspaceHandle` returned by
`content_graph.export_workspace(...)` and call `workspace.import_changes()`.
Create-only inline input uses `content_graph.import_inline(...)`; a project-local
single file uses `content_graph.import_file(...)`. Absolute paths and the raw
`content_graph.import_workspace(target, source, limits?)` carrier stay inside
SDK/server custody.

A StringTable export exposes an ordinary keyed entry document. Edit that document in place:

~~~hjson
namespace: "UI"
entries: {
  Start: {source: "Start", metadata: {Note: "Main menu"}}
  Exit: {source: "Exit"}
}
~~~

Entry keys retain their exact spelling and case. When `entries` is present it describes the desired
key set: adding a key creates an entry and removing a key deletes it. For an existing key, omitted
`source` or `metadata` retains the exported value. An explicit metadata object replaces that entry's
metadata; `{}` clears it. Metadata names follow native UE FName identity, so case-colliding names
are rejected. A namespace-only edit preserves entries; omitted namespace preserves its value.

Directory edits recover the target and concurrency baseline from the hidden workspace manifest.
Existing StringTable updates use the common atomic operation and persistence lifecycle, including
native package replacement before final readback. A failed update restores the pre-operation dirty
memory state and the original disk state separately. Persistence and readback evidence remain internal;
successful semantic directory import returns `status: succeeded` with `workspace_refresh`.

For a missing StringTable, pass this create-only document to `import_inline` or `import_file`:

~~~hjson
schema: "uai.string_table.edit/1"
root: {
  type: "/Script/Engine.StringTable"
  namespace: "UI"
  entries: {
    Start: {source: "Start", metadata: {Note: "Main menu"}}
  }
}
~~~

Initialization uses the same entry keys and literal text rules as directory editing. Each supplied
entry requires a string `source`; metadata values are strings, including values that resemble type
markers or references. Omitted `namespace` uses the native asset name, and omitted `entries` creates
an empty table. An explicitly empty namespace or source remains empty. The SDK/server supplies the
missing-target context; do not add a revision. An existing target must be edited through its exported
workspace. Creation is prepared before mutation and uses the common persistence lifecycle; failure
restores the original absence of the asset and its disk artifacts.

LocalizationSettings uses a semantic workspace for project (Game) targets. Its target is the native
settings object `/Script/Localization.Default__LocalizationSettings`, rather than a Content asset.
Export it with `client.content_graph.export_workspace("/Script/Localization.Default__LocalizationSettings", name="localization_settings")`.
To edit an existing target, change its ordinary `targets` entry, for example:

~~~hjson
targets: {
  Game: {native_culture: "en", cultures: ["en", "fr"]}
}
~~~

The target name is the collection key. Omitted fields preserve their captured values. Unedited targets
retain their complete native state, including an unconfigured native culture. For an edited target, the
effective native culture must belong to its culture set. The entry's `settings` exposes other native
editable target settings through the common typed struct/property representation, including gather,
export and compile configuration. Edit the exported values; omitted settings retain their native
values. Target identity and culture indexing remain owned by the native target lifecycle.
Do not add an operation, schema, revision, or internal target identifier to the ordinary document.

Add a new key to create a project target, for example `UI: {native_culture: "en", cultures: ["en", "fr"]}`.
UE creates the target identity. Names must be valid single directory names; an existing target or
occupied localization script/data path is rejected. Creation writes the native configuration and
formal scripts, without manufacturing a localization data directory.
For a new target, omitted culture fields use the current Editor culture. Specify both fields when
the target must use a particular language set.

When `targets` is present, its keys describe the desired target set: removing a key deletes that
project target, its eight native scripts, and its complete localization data directory. Omitting
`targets` preserves the collection. Directory removal includes nested files and empty directories;
trees or path ancestors containing symbolic links or Windows reparse points are rejected before writing.

Settings persist in the native default Editor configuration, together with the target's native
localization scripts. Updates use the common file transaction and independent final configuration
readback. Failure restores the original in-memory settings and disk state separately. A rejected
removal restores the same native target object, original configuration and scripts, and complete data tree.

StringTable and LocalizationSettings exports use semantic composition and the hidden workspace
manifest. The public SDK export returns a managed workspace handle; successful directory
import refreshes its standard HJSON and current baseline. Receipts and semantic hashes remain
hidden machine evidence.

## Read-only compound calls

Use a resolvable UE target and input
{"localization_capability":"localization.culture.read","parameters":{}}. Other capabilities use
their own target, expected, cultures, or entries parameters. The response declares read_only=true
and returns the observation in query_result.

## Gather, compile, and PO export

Start job.compile_build with family external_process, subject localization:<Target>, one
exact policy above, and parameters. Parameters accept execute, optional cultures[], and
an optional nonnegative timeout_seconds. Omission or zero lets the owned process run to completion. execute=true starts the
selected operation in the owned process. PO
export also requires a short project-relative `output_dir`, whose absolute wire
value is resolved privately by the SDK. execute=false produces a managed plan. Arbitrary executable and
argument fields are forbidden.
Exported files use `<output_dir>/<culture>/<native target PO filename>`, including when a subset
of cultures is requested. The directory is not passed to UE as a single-culture filename.

Use the handle's `finish()` to collect the receipt and release custody, or
`close()` to cancel and drain owned work. The report schema is
uai.localization.owned_tool_report.v1: compile returns locres_files[]/compiled_count, export returns
po_files[]/entry_count, and gather returns commandlet plan plus log/report paths. Generated configs
and owned processes are cleaned on release and abnormal destruction. A commandlet failure remains
a typed terminal failure.
The structured report is finalized before the terminal artifact is published.
Releasing the operation disposes its resources and does not change that artifact.

## Culture preview

Call editor.session("runtime_preview", "culture:<Culture>",
"policy:runtime-session:culture-preview/v1",
{"culture":"<Culture>","restore_after":true,"scope":"session"}). This exact policy uses an Editor
execution context, not a PIE subject. With restore_after=true the owned action sets the requested
culture and restores the previous culture. Its signal records
culture-preview:<previous>:<requested>:restored=true; localization.culture.read must independently
confirm the restored value. Invalid input is a typed terminal failure. Receipt and release remain
required.

## PO import

`external.localization.po.import(source_ref, target_ref)` is exposed as
`client.external.localization_po_import(project_relative_source, target_ref)` in the generated Python SDK. It
validates the PO header and target,
snapshots owned files, atomically stages the canonical PO, runs non-interactive import and
compile commandlets, refreshes resources, and independently verifies the persisted resources.
Confirmed restoration is rolled_back; unconfirmed restoration is state_unknown. The public success
payload contains only terminal status and the short target/culture, format, and importer,
compiler, and refresh business fields. Internal file paths, hashes, refresh revisions, receipts,
phase history, and full independent readback remain server evidence.

## Legacy exit

None of the 15 historical IDs is active in the manifest, SDK binding, registries, or compatibility
aliases. Historical ExternalDataImport and destructive-import code is not a fallback owner.
