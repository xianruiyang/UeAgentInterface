# T34 Consolidated Control Commands

`editor-session-build-command-owner` is the sole active owner of four public
commands: `editor.session`, `editor.safe_exit`, `job.compile_build`, and
`operation.plan.execute`. The first three start or schedule real Editor/build
mechanics. The fourth executes one preflighted plan through registered
providers; it does not create a second command registry or a second transport.

The public caller requires `uai:operate`. `editor.session` and
`job.compile_build` return a handle in `start_result.operation`. The SDK registers
recovery before sending; use `client.operation.named(...)` for a readable recovery
name. `asset_editor/open` is persistent:
advance until `payload.latest_signal.payload.phase == "active"`, then retain
the handle for interaction. It does not become terminal merely because the
asset or map opened. On stop, cancel, advance to terminal, read the receipt and
release through T32 `operation.*`. Finite operations advance directly to terminal.
`state_unknown` forbids automatic replay.

`pcg_generation` with `policy:pcg-generation:generate/v1` is finite. Use
`pcg-component:<exact loaded component_path>` and empty parameters, then
`started.operation.finish()`. The native completion event belongs to this
generation, including when inputs are unchanged. Successful generated content
survives release; saving remains a separate Editor operation. See
[PCG generation](t52_pcg_command_migration.md#explicit-editor-generation).

`asset-editor:open/v1` accepts optional `ownership: "managed" | "adopt"`.
The default `managed` owns windows opened by UAI (including implicit previews)
and follows native asset/editor replacements across imports. When a native
toolkit switches documents during a UAI open, its established ownership carries
across the old document's removal and the new document's registration. Only
registrations introduced by that open extend the allowed asset set. Existing user
windows are borrowed and stay open when the operation closes. `adopt` explicitly
takes closing responsibility for the asset's existing editors; use it only when
that responsibility is intended, including recovery of an older orphaned window.
Concurrent sessions share a managed resource; its final holder closes it.

Implicit preview/configuration/action/capture opens use the same ownership
registry. Their existing persistent-preview behavior is unchanged: completing
an action/capture does not close the preview. A subsequent `asset_editor/open`
session can acquire the managed window and close it through its operation handle.

Native close return values are not cleanup evidence. A remaining owned editor
keeps the operation nonterminal with `phase="cleanup_pending"`, the asset path
and a specific diagnostic. Unsaved changes are neither saved nor discarded by
cleanup. Resolve them or close the editor, then recover the existing operation
and call `close()` again. Repeating cancel retries stop, never replays start.
Autonomous observation confirms closure without repeatedly displaying dialogs.

## Public SDK and CLI

Import only `uai`, use `uai.connect_project()` for the local project, then use the
generated default methods:

```python
import uai

with uai.connect_project() as client:
    session = client.editor.session(
        "asset_editor",
        "asset:/Game/MyAsset.MyAsset",
        "policy:asset-editor:open/v1",
        {},
    )

    build = client.job.compile_build(
        "packaging",
        "project:current",
        "policy:packaging:query-environment/v1",
        {"include_sdks": True, "include_tools": True},
    )

    plan = client.operation.plan_execute(
        [
            {
                "step_id": "status",
                "kind": "transaction_history",
                "action": "status",
                "subject_ref": "editor:transaction-history",
                "policy_id": "policy:transaction-history:standard/v1",
                "parameters": {},
            }
        ],
        "stop",
        "reject_non_atomic",
    )

    # editor.safe_exit remains a generated exact binding, but ordinary callers
    # must use scripts/ue/SafeExitLifecycle.py to create its external witness.
```

The generated catalog exposes the exact
`client.editor.safe_exit(dirty_resolution, external_witness)` binding, but it
is not a direct lifecycle entry point. After exact dirty resource readback and
an explicit `reject|save|discard` decision, use the project
`scripts/ue/SafeExitLifecycle.py` runner. The runner owns the external process
witness and durable terminal; callers must not construct `external_witness`
themselves.

The `asset_compile` standard policy accepts empty parameters. Blueprint uses the
same compiler/report owner as `policy:asset-compile:blueprint/v1`, includes native
messages, and does not save. StaticMesh supplies UE's native diagnostics buffer,
which also waits for that mesh's compilation; its report exposes `build_completed`,
`diagnostic_count`, and `messages`. UE does not attach severity to that buffer and
can include warnings, so a nonzero count is not an automatic build failure.
Both reports use the existing operation artifact publisher. Blueprint-specific
options belong to the Blueprint policy and cannot compile a different asset type.
Compilation completion is separate from content validity and persistence.

The packaging family uses exact policies for `query-environment`,
`export-profile`, `validate-profile`, `diff-profile`, `plan-build`,
`run-build`, `parse-log`, `report-artifacts`, and `clean-artifacts`. Static
policies publish a JSON `report_ref`; `run-build` launches only the trusted
structured `RunUAT BuildCookRun` plan under the operation-owned process and
publishes its log/report reference. Use the returned handle's `finish()` to
collect and release its receipt, or `close()` to cancel and drain it.

`operation.plan.execute` preflights every exact step before execution. Its providers are:

- `public_command`: exact current command ID plus exact binding hash; recursive
  invocation of `operation.plan.execute` is rejected.
- `editor_console`: server-owned console execution with an explicit policy for commands that can
  close, travel, delete, restart, or force garbage collection.
- `transaction_history`: `status`, `undo`, or `redo` against
  `editor:transaction-history`; mutations require the observed revision.

All steps are preflighted before execution. `reject_non_atomic` accepts only a
single transaction-history mutation; mixed plans use explicit `best_effort`
semantics and report every completed/failed step in
`uai.operation.plan.receipt.v1`.

The CLI forwards the same generated catalog and exact hash:

```text
ueagentinterfacecmd exec operation.plan.execute --params-json '{"steps":[{"step_id":"status","kind":"transaction_history","action":"status","subject_ref":"editor:transaction-history","policy_id":"policy:transaction-history:standard/v1","parameters":{}}],"on_error":"stop","atomicity":"reject_non_atomic"}'
```

For unknown command names or parameter shapes, follow [public command discovery](../../../../UeAgentInterfaceCMD/docs/PYTHON_SDK_AND_CLI.md#command-discovery). Generic
`client.execute(...)`, exact `client.execute_exact(...)`, named SDK calls, and
`ueagentinterfacecmd` all use the same immutable registry and transport. No T34-specific
Python runtime, alias, polling loop, process manager, or lifecycle authority is
introduced.
