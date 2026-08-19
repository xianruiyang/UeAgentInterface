# T51 Niagara command authority

The current default methods are:

```python
client.niagara.asset_duplicate(...)
client.niagara.compile_refresh_settle(...)
client.niagara.compound_apply(...)
client.niagara.emitter_asset_create(...)
client.niagara.renderer_event_parameter_apply(...)
client.niagara.stack_issue_fix(...)
client.niagara.stack_reorder(...)
client.niagara.system_asset_create(...)
```

They are discovered by command_search and command_detail and mirrored by
uai-cli exact execution. The Niagara command owner delegates all native asset,
graph, stack, compile/settle, persistence, readback, and cleanup mechanics; no
task-specific generated projection remains active.

`niagara.script.graph.apply` and `niagara.stack.apply` are replaced public
identities and are not registered, exported in the formal manifest, or exposed
by the default SDK. T51 retains only the typed Niagara mechanics used by the
eight commands above. Niagara HJSON authoring is contributed by the T18 domain
provider behind the single public `content_graph.import_workspace` ingress; it
consumes T13's common initialization, failure, rollback, and cleanup contracts,
and must not restore either replaced ID or a second HJSON runtime.

Terminal success includes required native settle and independent persisted
readback. Invalid or stale input fails before mutation; unknown state is
reconciled. Cleanup touches only owned fixtures and is independently verified.
