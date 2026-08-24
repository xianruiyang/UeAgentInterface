# T34 Consolidated Control Commands

`editor-session-build-command-owner` is the sole active owner of four public
commands: `editor.session`, `editor.safe_exit`, `job.compile_build`, and
`operation.plan.execute`. The first three start or schedule real Editor/build
mechanics. The fourth executes one bounded, preflighted plan through registered
providers; it does not create a second command registry or a second transport.

The public caller requires `uai:operate`. `editor.session` and
`job.compile_build` return an `operation_ref`; observe, cancel when supported,
read the terminal receipt, and release it through the T32 `operation.*`
lifecycle. `state_unknown` forbids automatic replay.

## Public SDK and CLI

Import only `uai`, connect from one complete Connection TOML, then use the
generated default methods:

```python
import uai

with uai.connect("connection.toml") as client:
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

The packaging family uses exact policies for `query-environment`,
`export-profile`, `validate-profile`, `diff-profile`, `plan-build`,
`run-build`, `parse-log`, `report-artifacts`, and `clean-artifacts`. Static
policies publish a JSON `report_ref`; `run-build` launches only the trusted
structured `RunUAT BuildCookRun` plan under the operation-owned process and
publishes its log/report reference. Use `operation.status`,
`operation.receipt`, `operation.cancel`, and `operation.release` with the
returned exact operation reference.

`operation.plan.execute` accepts at most 128 exact steps. Its providers are:

- `public_command`: exact current command ID plus exact binding hash; recursive
  invocation of `operation.plan.execute` is rejected.
- `editor_console`: bounded console command/output with explicit risk policy.
- `transaction_history`: `status`, `undo`, or `redo` against
  `editor:transaction-history`; mutations require the observed revision.

All steps are preflighted before execution. `reject_non_atomic` accepts only a
single transaction-history mutation; mixed plans use explicit `best_effort`
semantics and report every completed/failed step in
`uai.operation.plan.receipt.v1`.

The CLI forwards the same generated catalog and exact hash:

```text
uai-cli --connection connection.toml exec operation.plan.execute --params-json {"steps":[{"step_id":"status","kind":"transaction_history","action":"status","subject_ref":"editor:transaction-history","policy_id":"policy:transaction-history:standard/v1","parameters":{}}],"on_error":"stop","atomicity":"reject_non_atomic"}
```

Use `client.query.command_search("editor session packaging operation plan",
limit=10)` and then `client.query.command_detail(command_id, contract_hash)`
for the exact schema, owner, failures, and tutorial reference. Generic
`client.execute(...)`, exact `client.execute_exact(...)`, named SDK calls, and
`uai-cli` all use the same immutable registry and transport. No T34-specific
Python runtime, alias, polling loop, process manager, or lifecycle authority is
introduced.
