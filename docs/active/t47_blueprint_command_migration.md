# T47 Blueprint command authority

T47 owns exactly two current Blueprint-specific non-HJSON commands:

```python
client.blueprint.asset_create(...)
client.blueprint.component_events_discover(...)
```

Use `client.command_search(...)` and `client.command_detail(...)` to obtain the
current exact parameter schema and binding hash. The same commands are available
through `uai-cli ... exec <command-id>`. The generated SDK forwards one exact
manifest request and does not compile, save, open editors, inspect components,
or manipulate graph/viewport state in Python.

`blueprint.component.events.discover` consumes `asset_path`,
`compile_before_query`, `component_name`, `include_hidden`, `limit`,
`only_unbound`, and `query`. It resolves a real Actor Blueprint component and
enumerates its `BlueprintAssignable` multicast delegates, including current
bound-event state, stable filtering, and a bounded result count.

## Consolidated Blueprint workflows

Blueprint compile and compile-log behavior uses `client.job.compile_build(...)`
with family `asset_compile` and policy
`policy:asset-compile:blueprint/v1`. Its policy parameters are `asset_path`,
`include_messages`, `max_messages`, `save_after_compile`, and
`severity_filter`. Follow the returned operation with the normal
`operation.status`, `operation.receipt`, and `operation.release` commands. The
terminal `report_ref` identifies the bounded typed compile report; saving is
verified against the package file and by an independent Blueprint reload.

Blueprint graph live and persisted view uses `graph.view.read` and
`graph.view.update` with the exact `UEdGraph` object path as both target and
`context.asset_editor_identity`. A persisted update sets
`update_document_state`; `save_after_update` additionally saves the Blueprint.
Persisted location is deterministically pixel-snapped at the requested zoom and
is read back from `LastEditedDocuments` after unload/reload. These commands do
not perform graph layout.

Blueprint Components preview camera uses `viewport.state.read` and
`viewport.state.update`. Supply all of the following exact target fields:

- `target` and `context.viewport_identity`:
  `asset:<BlueprintObjectPath>#blueprint_components`;
- `context.asset_editor_identity`: the exact Blueprint object path;
- `context.viewport_scope`: `asset_editor`;
- `context.viewport_role`: `components`;
- `context.world_type`: `editor`.

`state_patch.camera_transform` carries location and rotation;
`state_patch.fov_degrees` carries FOV. Resolution only uses a registered live
preview session or an already-live Components tab. It never opens an asset
editor, switches mode, invokes a tab, activates a window, or takes focus.

The historical IDs `blueprint_compile`, `blueprint_get_compile_log`,
`blueprint_graph_get_view`, `blueprint_graph_set_view`,
`blueprint_list_component_events`, `blueprint_viewport_get_camera`, and
`blueprint_viewport_set_camera` are absent from the active manifest, generated
SDK, aliases, and executable routes. Their exact successor and evidence
contracts are recorded in `successors/t47_blueprint_command_migration.json`.
