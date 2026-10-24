# T47 Blueprint command authority

T47 owns exactly two current Blueprint-specific non-HJSON commands:

```python
client.blueprint.asset_create(...)
client.blueprint.component_events_discover(...)
```

For unknown command names or parameter shapes, follow [public command discovery](../../../../UeAgentInterfaceCMD/docs/PYTHON_SDK_AND_CLI.md#command-discovery).
Binding hashes remain SDK-owned. The same commands are available
through `ueagentinterfacecmd ... exec <command-id>`. The generated SDK forwards one exact
manifest request and does not compile, save, open editors, inspect components,
or manipulate graph/viewport state in Python.

`blueprint.component.events.discover` consumes `asset_path`,
`compile_before_query`, `component_name`, `include_hidden`,
`only_unbound`, and `query`. It resolves a real Actor Blueprint component and
enumerates its `BlueprintAssignable` multicast delegates, including current
bound-event state and stable filtering, and returns every matching result.

## Consolidated Blueprint workflows

Successful Blueprint-root HJSON import already includes compile, save, reload,
and persisted readback; it does not need an additional compile job for those
steps. Blueprint-family publication preserves required persistent subobjects
without promoting intentionally transient optional objects. Missing required
objects, incorrect ownership, or unsaveable required objects produce structured
diagnostics with the target, property and observed/expected state before saving.
For Widget Blueprints, an empty source WidgetTree is valid; a missing source
WidgetTree is a structural error, not an empty document.

Independent Blueprint compile reports use `client.job.compile_build(...)`
with family `asset_compile` and policy
`policy:asset-compile:blueprint/v1`. The job subject is `asset:<asset_path>`
(for example `asset:/Game/Example/BP_Item.BP_Item`), while `parameters.asset_path`
is the unprefixed exact object path. Its policy parameters are `asset_path`,
`include_messages`, `save_after_compile`, and
`severity_filter`. Keep the returned typed operation handle alive and call
`resume()` until the returned snapshot has `terminal=true`; `status()` only
observes and does not advance execution. Then call `receipt()` and `release()`.
Use terminal-result or receipt `artifacts` and `materialize("tmp/<run>/compile.json")`
to read the compile report. `native_exited_success` is execution completion;
check the report's `has_error/error_count` for Blueprint compilation success.
This standalone policy's save result checks the native package save, dirty state,
and package file. The independent reload belongs to the HJSON import lifecycle.

The generic `policy:asset-compile:standard/v1` with empty parameters also emits
the same Blueprint report (including native messages, without saving). It uses
the same compiler/report owner; it is not a second Blueprint compile path.

Blueprint graph live and persisted view uses `graph.view.read` and
`graph.view.update` with the exact `UEdGraph` object path as both target and
`context.asset_editor`. A persisted update sets
`update_document_state`; `save_after_update` additionally saves the Blueprint.
Persisted location is deterministically pixel-snapped at the requested zoom and
is read back from `LastEditedDocuments` after unload/reload. These commands do
not perform graph layout.

Blueprint Components preview camera uses `viewport.state.read` and
`viewport.state.update`. Supply all of the following exact target fields:

- `target` and `context.viewport`:
  `asset:<BlueprintObjectPath>#blueprint_components`;
- `context.asset_editor`: the Blueprint object path;
- `context.viewport_scope`: `asset_editor`;
- `context.viewport_role`: `components`;
- `context.world_type`: `editor`.

`state_patch.camera_transform` carries location and rotation;
`state_patch.fov_degrees` carries FOV. Resolution only uses a registered live
preview session or an already-live Components tab. It never opens an asset
editor, switches mode, invokes a tab, activates a window, or takes focus.
