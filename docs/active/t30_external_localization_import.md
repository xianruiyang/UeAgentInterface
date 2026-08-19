# T30 Localization / StringTable

T30 closes the 15 historical Localization and StringTable identities without restoring their old
names. The exact line-by-line disposition is
[successors/t30_external_localization_import.json](successors/t30_external_localization_import.json).

Callers that start from intent use the public `query.command.search` / SDK
`client.query.command_search(...)` route, then `query.command.detail` / SDK
`client.query.command_detail(command_id, contract_hash)` with the returned command ID and contract
hash. The equivalent CLI surface is `uai-cli`; use its query search/detail commands for discovery
and `uai-cli ... exec <command-id> --params-json <json>` for execution. Discovery does not execute
or replace any route listed below.

## Public routing

| Historical capability | Current command | Exact provider, capability, or policy |
|---|---|---|
| localization_apply_json | content_graph.import_workspace | schema uai.localization.edit/1 |
| string_table_apply_json | content_graph.import_workspace | schema uai.string_table.edit/1 |
| localization_export_json / string_table_export_json | content_graph.export_workspace | verified workspace publisher |
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

The only import command is content_graph.import_workspace(target, source, expected_revision,
authorization_id). Source is exactly one of {kind:"inline", document:"..."},
{kind:"single_file", path:"..."}, or {kind:"directory", path:"..."}.

A StringTable document replaces its complete namespace and entry set:

~~~json
{"schema":"uai.string_table.edit/1","root":{"type":"/Script/Engine.StringTable","namespace":"UI","entries":[{"key":"Start","source":"Start"}]}}
~~~

The first create uses expected_revision="absent"; updates use the exact revision returned by a
current summary/export. Save failure restores the previous package or unregisters and removes a
newly created table. Success includes a persisted revision and independent entry readback.

Localization settings are always present, so expected_revision="absent" is rejected. Every target
item declares create, update, upsert, or remove:

~~~json
{"schema":"uai.localization.edit/1","root":{"type":"/Script/LocalizationDashboard.LocalizationSettings","targets":[{"operation":"create","name":"Game","native_culture":"en","cultures":["en","fr"]}]}}
~~~

Create/upsert of an absent target requires native_culture and cultures; update preserves omitted
fields. The effective native culture must be in the effective culture set. The provider validates
the full batch, stages all target configs, then commits. A later I/O failure restores earlier
configs and leaves no newly created target. Remove deletes the exact target config and owned
Content/Localization/<Target> directory; rollback failure is state_unknown.

content_graph.export_workspace publishes the same schemas through the verified workspace
publisher. Its receipt and semantic hashes, not old JSON field names, define successful export.

## Read-only compound calls

Use a resolvable UE target and input
{"localization_capability":"localization.culture.read","parameters":{}}. Other capabilities use
their own target, expected, cultures, or entries parameters. The response declares read_only=true
and returns the observation in query_result.

## Gather, compile, and PO export

Start job.compile_build with family localization_owned_tool, subject localization:<Target>, one
exact policy above, and parameters. Parameters accept execute, optional cultures[], and
timeout_seconds in (0,300]. Gather and compile require allow_commandlet=true when execute=true; PO
export also requires output_dir. execute=false produces a managed plan. Arbitrary executable and
argument fields are forbidden.

Advance only with operation.resume(operation_ref, expected_generation); status is read-only. At
terminal, read operation.receipt and always call operation.release. The report schema is
uai.localization.owned_tool_report.v1: compile returns locres_files[]/compiled_count, export returns
po_files[]/entry_count, and gather returns commandlet plan plus log/report paths. Generated configs
and owned processes are cleaned on release and abnormal destruction. A commandlet failure remains
a typed terminal failure.

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
`client.external.localization_po_import(source_ref, target_ref)` in the generated Python SDK. It
validates the PO header and target,
snapshots owned files, atomically stages the canonical PO, runs bounded non-interactive import and
compile commandlets, refreshes resources, and independently hashes PO/archive/locres. Confirmed
restoration is rolled_back; unconfirmed restoration is state_unknown.

## Legacy exit

None of the 15 historical IDs is active in the manifest, SDK binding, registries, or compatibility
aliases. Historical ExternalDataImport and destructive-import code is not a fallback owner.
