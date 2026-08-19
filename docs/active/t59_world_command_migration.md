# T59 World command authority

T59 owns these eleven typed World mutations:

```python
client.world.actor_create(...)
client.world.actor_hlod_layer_update(...)
client.world.actor_streaming_policy_update(...)
client.world.data_layer_instance_create(...)
client.world.data_layer_relation_update(...)
client.world.data_layer_state_update(...)
client.world.level_game_mode_update(...)
client.world.streaming_relation_create(...)
client.world.streaming_relation_state_update(...)
client.world.wall_create_with_opening(...)
client.world.world_partition_streaming_update(...)
```

Discover an unknown operation with `client.query.command_search(intent, limit=...)`, then inspect
the returned exact ID and contract hash with
`client.query.command_detail(command_id, contract_hash)`. `uai-cli` exposes the same
`command_search`, `command_detail` and exact execution path. Discovery never executes a command.

## HLOD Layer HJSON

`content_graph.import_workspace` is the only public HJSON import command. HLOD Layer accepts the
same `inline`, `single_file` and `directory` source union as every other domain:

```python
result = client.content_graph.import_workspace(
    "/Game/World/HLOD/HLOD_Instancing.HLOD_Instancing",
    {"kind": "directory", "path": r"D:\\workspaces\\hlod_instancing"},
    expected_revision,
    authorization_id="authorization:hlod-layer:write",
)
```

The document root type is `/Script/Engine.HLODLayer`. The shared import preflight validates the
complete source, schema, references and recursive initialization before mutation. Success requires
save, reload and independent `client.content_graph.summary(...)` readback. Export uses
`client.content_graph.export_workspace(target, workspace_path, profile_id="llm_authoring")`.
There is no HLOD apply-file or apply-folder command.

## Read-only World Partition and streaming state

```python
partition = client.world.partition(
    input={"include_loaded_regions": True}, context={}, projection={}
)
health = client.world.partition_health(
    input={"enable_streaming": True}, context={}, projection={}
)
streaming = client.world.streaming_levels(input={}, context={}, projection={})
hlod = client.world.hlod_assignments(input={}, context={}, projection={})
```

`world.partition_health` calls the real World Partition error checker and returns
`health_status`, `error_count` and issue entries. These four commands are Query operations: they do
not create jobs, save packages or claim a lifecycle terminal state.

## Level sessions and World Partition loaders

Create a blank persistent map with a recoverable session:

```python
start = client.editor.session(
    "world_context",
    "world:/Game/Maps/LV_Test",
    "policy:world-context:create-blank/v1",
    {
        "overwrite_policy": "fail",
        "save_existing_map": False,
        "create_world_partition": True,
    },
)
operation_ref = start.payload["operation_ref"]
generation = start.payload["generation"]
status = client.operation.resume(operation_ref, generation)
```

Open an existing map with `policy:world-context:set-current/v1`. Region and actor loaders use
`policy:world-context:partition-region-loader/v1` and
`policy:world-context:partition-actor-loader/v1`. A loader remains owned by its `operation_ref`;
observe it with `operation.status`, unload it with `operation.cancel`, wait for the cancelled
terminal state, then call `operation.release`. There is no domain loader registry or second loader
ID source.

## HLOD, navigation and partition jobs

```python
start = client.job.compile_build(
    "world_build",
    "world:/Game/Maps/LV_Test",
    "policy:world-build:hlod-plan/v1",
    {"step": "build"},
)
status = client.operation.resume(
    start.payload["operation_ref"], start.payload["generation"]
)
```

HLOD execute uses `policy:world-build:hlod-run/v1`; accepted steps are `setup`, `build`,
`finalize`, `delete` and `stats`. Execute requires explicit commandlet and destructive
authorization, and rendering/RHI authorization where the selected step needs them. Navigation uses
`policy:world-build:navigation/v1`. World Partition streaming generation uses
`policy:world-build:partition-streaming-plan/v1` and its run peer. Exact Actor-GUID resave uses
`policy:world-partition-resave:actors-plan/v1` and
`policy:world-partition-resave:actors-run/v1`. Plan returns a durable report without executing; run uses one owned,
unattended `UnrealEditor-Cmd` process and exposes its report and process return code through the
common operation lifecycle.

## Completion semantics

A successful start is not success. Advance only with `operation.resume` and its exact generation;
use `operation.status` for observation. Mutation success is emitted only after required validation,
real UE mechanics, persistence and independent readback. Wall geometry keeps
`semantic_frame_id`, write/read/verify frames, units, basis evidence and conversion chain explicit.
Prewrite failure leaves no residue, unknown post-write state is reconciled, and cleanup is bounded.

The complete 26-row historical mapping is
`docs/active/successors/t59_world_command_migration.json`. Every old command ID is absent from the
active manifest, generated SDK and compatibility aliases; the mapping identifies the exact current
command set or architecture contract for each frozen purpose, input and output.
