# T39 EditorProject Operation Authority

The `editor-project-operation` contribution owns exactly the seven
`editor_project.*.update` identities assigned to its domain. Five remain active:
config-section, device-profile, platform-config, project-settings and scalability-group
updates. Localization-target and StringTable updates are centrally dormant because
their registered HJSON profiles are now the normal authoring surface. Every
frozen contract reports `editor-project-operation-owner`; no namespace or
catch-all fallback adds commands.

The exact handlers require `uai:operate`, add only the private
`uai:editor-project:write` mechanics scope and forward one request to the common
OperationService. The EditorProject simple-effect adapter remains the sole owner
of the native config, settings, localization and StringTable mechanics. It is an
internal dependency, not a second public route, registry, handler, contract,
test or documentation authority.

The common lifecycle performs schema validation, exact target and field
resolution, server-owned current-revision binding, native execution, settle,
persistence, reload, independent readback
and rollback or `state_unknown` handling. A stale revision or invalid typed
change fails before mutation; `state_unknown` is not safe for automatic replay.

## Public SDK and CLI

The generated catalog retains all seven identities. Only the five active
identities belong in the named methods of the default `editor_project`
namespace. Callers import only `uai`, use `uai.connect_project()` for the local project and
call methods such as `client.editor_project.config_section_update(...)`. Localization
and StringTable edits use `workspace = client.content_graph.export_workspace(...)`,
ordinary HJSON editing and `workspace.import_changes()`; their old exact handlers
remain compatibility-only behind the central dormant lifecycle. Generic
`client.execute(...)`, exact compatibility dispatch, and `ueagentinterfacecmd exec` reuse the same
generated catalog, Client, Protocol and Transport. No T39-specific Python module
remains authoritative.

The five active update methods accept only the readable `target_ref` and typed
`changes`. The EditorProject target resolver reads the current revision and
freezes it inside the same Operation before planning; later prestate and
prewrite checks still reject an internally changed target. The revision is not
a Python/HJSON/CLI parameter. `reference.catalog_search` is retired and must
not be restored as a second target directory.

For example, a config-section target is read through `config.section`:

```python
query_result = client.config.section(
    input={
        "editor_project_capability": "target.read",
        "parameters": {"target_ref": target_ref},
    },
)
# Use query_result.payload["observation"]["entries"] to choose the typed change.
```

The same `target.read` action is closed over `config.section`,
`config.resolved_value`, `project.settings`, `project.platform`, and
`project.validation`. Project-settings observations additionally return
`fields[]` with exact field ref, type and current canonical value. Target and
field revisions remain hidden inside the same server-owned operation. Explicit
capability requests return `observation`; ordinary queries retain their compact
sections/settings/values results. After
the update, repeat `target.read` for independent product readback.

```text
ueagentinterfacecmd exec editor_project.config_section.update --params-json '{"changes":[{"field_id":"entries","presence":"set","value":{"type_id":"uai.editor_project.config_entry_map.v1","canonical_value":{"UAI_StrictV2_T39":"public-write"}}}],"target_ref":"uai://editor-project/v1/config-section/game/%2FScript%2FEngineSettings.GeneralProjectSettings"}'
```

For unknown command names or parameter shapes, follow [public command discovery](../../../../UeAgentInterfaceCMD/docs/PYTHON_SDK_AND_CLI.md#command-discovery).
Search matches only positive tokens from command IDs, registered keywords and
registered synonyms; call the selected generated method by name.

## Historical read capabilities

The fifteen retained historical read/diff/validate behaviors are closed
capability actions on the five Query commands, not public aliases:

- `config.section`: `device_profile.export`, `device_profile.query`,
  `scalability.export`;
- `config.resolved_value`: `device_profile.resolve_cvars`;
- `project.settings`: `project_settings.index`, `project_settings.page`;
- `project.platform`: `platform_config.export`, `target_platform.query`;
- `project.validation`: `device_profile.diff`, `device_profile.validate`,
  `platform_config.validate`, `platform_profile.validate_packaging`,
  `project_settings.diff`, `project_settings.validate`,
  `scalability.validate`.

Put the exact action in `input.editor_project_capability` and its historical
parameters in `input.parameters`. File-output parameters are provenance only:
the Query route forces `write_file=false` and `write_files=false` and never
writes a server-side export file.
