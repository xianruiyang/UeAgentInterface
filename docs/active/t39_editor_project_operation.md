# T39 EditorProject Operation Authority

The active `editor-project-operation` contribution owns exactly the seven
`editor_project.*.update` identities assigned to
`T39-EDITOR-PROJECT-OPERATION` in `UAI_COMMAND_TASK_INVENTORY.json`. Every
frozen contract reports `editor-project-operation-owner`; no namespace or
catch-all fallback adds commands.

The exact handlers require `uai:operate`, add only the private
`uai:editor-project:write` mechanics scope and forward one request to the common
OperationService. The EditorProject simple-effect adapter remains the sole owner
of the native config, settings, localization and StringTable mechanics. It is an
internal dependency, not a second public route, registry, handler, contract,
test or documentation authority.

The common lifecycle performs schema validation, exact target and revision
binding, native execution, settle, persistence, reload, independent readback
and rollback or `state_unknown` handling. A stale revision or invalid typed
change fails before mutation; `state_unknown` is not safe for automatic replay.

## Public SDK and CLI

The final manifest projects all seven identities into the default
`editor_project` namespace. Callers import only `uai`, connect from one complete
Connection TOML and call methods such as
`client.editor_project.config_section_update(...)` and
`client.editor_project.string_table_update(...)`. Generic `client.execute(...)`,
exact `client.execute_exact(...)`, and `uai-cli exec` reuse the same generated
catalog, Client, Protocol and Transport. No T39-specific Python module remains
authoritative.

`expected_revision` is the exact revision of the selected target, not a retry
token or user-authored label. Obtain it immediately before writing with the
closed `target.read` action on the matching active Query command; never guess it
or reuse a stale value. `reference.catalog_search` is retired and must not be
restored as a second target directory.

For example, a config-section target is read through `config.section`:

```python
query_result = client.config.section(
    input={
        "config_scope": "game",
        "section_id": "/Script/EngineSettings.GeneralProjectSettings",
        "editor_project_capability": "target.read",
        "parameters": {"target_ref": target_ref},
    },
    projection={"max_items": 2},
)
# Parse the returned observation_json entry. It contains target_revision and
# independently loaded entries; use that revision in the update below.
```

The same `target.read` action is closed over `config.section`,
`config.resolved_value`, `project.settings`, `project.platform`, and
`project.validation`. Project-settings observations additionally return
`fields[]` with exact field ref, field revision, type and current canonical
value. After the update, repeat `target.read` for independent product readback.

```text
uai-cli --connection connection.toml exec editor_project.config_section.update --params-json {"changes":[{"field_id":"entries","presence":"set","value":{"type_id":"uai.editor_project.config_entry_map.v1","canonical_value":{"UAI_StrictV2_T39":"public-write"}}}],"expected_revision":"<current-target-revision>","target_ref":"uai://editor-project/v1/config-section/game/%2FScript%2FEngineSettings.GeneralProjectSettings"}
```

Use `client.query.command_search("update project config section", limit=5)` and
then `client.query.command_detail(command_id, contract_hash)` to obtain the exact
request schema, named SDK/CLI forms, owner, failures and tutorial references.
Search is the only fuzzy command-selection step; execute the selected ID and
hash unchanged.

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
