# T51 Niagara command authority

The current default methods are:

```python
client.niagara.asset_duplicate(...)
client.niagara.compile_refresh_settle(...)
client.niagara.emitter_asset_create(...)
client.niagara.stack_issue_fix(...)
client.niagara.system_asset_create(...)
```

`asset_duplicate(source_ref, target_ref)` accepts a package path or its matching
top-level object path for the destination, for example `/Game/VFX/NS_Copy` or
`/Game/VFX/NS_Copy.NS_Copy`. Both identify the same new asset package. A subobject
or a different object name is not an asset creation target. The source accepts
the existing asset's package or object path. Existing destinations are preserved
and reported as already present.

`compile_refresh_settle(target_ref)` accepts the existing asset's package or
object path. Persistence uses the resolved asset package for either spelling.

A standalone Emitter is compiled in UE's transient System context using its
exposed version. Success validates that compiled instance and settles/saves the
source definition; it does not copy instance VM bytecode back into the source.
Its read-only compile coverage is therefore `not_applicable` for independent
stored VM freshness. This is not an error or a claim that an uncompiled instance
is clean. System instances and compilable standalone scripts keep their own
stored compile diagnostics; actual messages and Stack issues remain visible.

`stack_issue_fix` accepts the diagnostic's `details.emitter`, `message`, and fix
`description` in its existing `emitter_handle_id`, `issue_id`, and `fix_id` string
parameters. Native IDs remain compatible for machine callers. Emitter names use
the shared authoring owner resolver; issue and fix labels require unique matches.
The operation searches all issue-bearing entries in that emitter Stack, matching
the diagnostic read scope. System-level issues are not emitter fix targets.

For unknown command names or parameter shapes, follow [public command discovery](../../../../UeAgentInterfaceCMD/docs/PYTHON_SDK_AND_CLI.md#command-discovery). The Niagara command owner delegates all native asset,
graph, stack, compile/settle, persistence, readback, and cleanup mechanics; no
task-specific generated projection remains active.

`niagara.script.graph.apply`, `niagara.stack.apply`,
`niagara.renderer_event_parameter.apply`, and `niagara.stack.reorder` are
replaced public identities and are not registered, exported in the formal
manifest, or exposed by the default SDK. Renderer event parameter editing and
stack ordering use Niagara HJSON authoring through the T18 domain provider and
the single public `content_graph.import_workspace` ingress. Their internal
operation kinds and native mechanics remain available to that provider; they
are not a second public command surface. `niagara.compound.apply` is a dormant
compatibility identity under the central command lifecycle policy; it is not
an ordinary-discovery or named-SDK fallback for incomplete HJSON capabilities.
Linked parameter coverage remains a separate HJSON obligation. The provider consumes T13's common initialization,
failure, rollback, and cleanup contracts.

Captured emitter struct properties now use the shared compact property path
described in T18. Standalone and unversioned System embedded `FixedBounds` cases
are verified; linked parameter, multi-version emitter, renderer, and data-interface
coverage remain separate obligations.

Terminal success includes required native settle and independent persisted
readback. Invalid or stale input fails before mutation; unknown state is
reconciled. Cleanup touches only owned fixtures and is independently verified.
