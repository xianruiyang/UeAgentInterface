# T59 World command authority

T59 keeps one typed World mutation active for procedural whitebox authoring:

```python
client.world.wall_create_with_opening(...)
```

The nine typed persistent World mutations for Actor HLOD/streaming policy, Data
Layer structure/state, level GameMode, streaming relations and World Partition
streaming are dormant compatibility identities. Their desired state belongs to
World HJSON; incomplete profile/adapter coverage remains an explicit capability
gap and does not justify exposing the old typed commands in ordinary discovery.

`world.actor.create` is also dormant and has no generated named SDK method. New persistent Actor creation uses
`world.actor.candidates` followed by a Level Actor HJSON create through
`content_graph.import_workspace`. Its provider extension carries the selected
candidate plus optional label, folder, transform, attachment and non-reserved
Actor tags; properties and components remain ordinary HJSON units.

Discover an unknown operation with `client.query.command_search(intent)`, then call
`client.query.command_usage(names)` for the minimum named SDK call. Use the generated SDK method for that command;
discovery never executes a command.

## HLOD Layer HJSON

HLOD Layer assets use the common Reflection semantic workspace. Export the real asset, edit only
the ordinary HJSON returned by that workspace, and import it through the workspace handle:

```python
workspace = client.content_graph.export_workspace(
    "/Game/World/HLOD/HLOD_Instancing.HLOD_Instancing",
    name="hlod_edit",
    profile_id="llm_authoring",
)
result = workspace.import_changes()
```

The workspace exposes editable `UHLODLayer` root properties and editable descendants of the existing
`HLODBuilderSettings` object. Its pointer is read-only: an import cannot clear it, replace its class,
or bind an external object. Native `LayerType`/`HLODBuilderClass` notifications derive the settings
object before its descendant edits are applied. Read the refreshed workspace after a type change for its editable
fields; a combined edit must use fields valid on the resulting native settings class. An explicit
`class_path` is an assertion, not permission to replace the read-only pointer. Empty
`owner_relative_path` cannot request a new settings object.

To create an absent HLOD Layer, import an ordinary initialization document through the same
`content_graph.import_workspace` command. The SDK/service owns revision and authorization; do not
provide machine-token fields. The source union accepts
`inline`, `single_file`, or a domain document directory. The document uses the shared Reflection
operation/target/properties contract:

Use `client.content_graph.import_inline(target_object_path, document_text)` for inline creation.

```hjson
schema: uai.world_hlod_layer.edit/1
root: {
  type: /Script/Engine.HLODLayer
  document: {
    operation: create
    target: {
      asset_path: /Game/World/HLOD/HLOD_Instancing
      asset_kind: hlod_layer
    }
    properties: [
      { property_path: "LayerType", value: "Instancing" }
      { property_path: "CellSize", value: 512 }
    ]
  }
}
```

This replaces the former HLOD-only `layer_type`/`cell_size` value parser. The native factory creates
the root and its defaults; common Reflection owns preview, recursive properties, transaction,
save/reload, final readback and absent-target recovery. Existing assets use exported semantic
workspaces. HLOD build and execution remain independent World commands.

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
handle = start.operation
status = handle.resume()
```

Open an existing map with `policy:world-context:set-current/v1`. Region and actor loaders use
`policy:world-context:partition-region-loader/v1` and
`policy:world-context:partition-actor-loader/v1`. A loader remains owned by its typed SDK handle;
observe it with `handle.status()` and unload/drain it with `handle.close()`.
There is no domain loader registry or second loader
ID source.

## HLOD, navigation and partition jobs

```python
start = client.job.compile_build(
    "world_build",
    "world:/Game/Maps/LV_Test",
    "policy:world-build:hlod-plan/v1",
    {"step": "build"},
)
receipt = start.operation.finish()
```

HLOD execute uses `policy:world-build:hlod-run/v1`; accepted steps are `setup`, `build`,
`finalize`, `delete` and `stats`. The exact run policy starts the owned commandlet; `delete` also
requires `allow_delete=true`. Build and finalize use the renderer, while the remaining steps use
NullRHI. Navigation uses
`policy:world-build:navigation/v1`. World Partition streaming generation uses
`policy:world-build:partition-streaming-plan/v1` and its run peer. Exact Actor-GUID resave uses
`policy:world-partition-resave:actors-plan/v1` and
`policy:world-partition-resave:actors-run/v1`. Plan returns a durable report without executing; run uses one owned,
unattended `UnrealEditor-Cmd` process and exposes its report and process return code through the
common operation lifecycle.

## Completion semantics

A successful start is not success. Use `handle.finish()` for finite work and
`handle.wait(until=...)` for persistent readiness. Mutation success is emitted only after required validation,
real UE mechanics, persistence and independent readback. Wall geometry keeps
`semantic_frame_id`, write/read/verify frames, units, basis evidence and conversion chain explicit.
Prewrite failure leaves no residue, unknown post-write state is reconciled, and cleanup is bounded.

The compatibility mechanics behind dormant `world.data_layer_relation.update`
treat direct Actor membership as two-package durable state:
the member Actor external package carries the serialized `DataLayerAsset` reference, while the
`AWorldDataLayers` external package carries the referenced instance and its private asset. Both are
enlisted through the native mutation facts before persistence. Independent World readback loads the
exact map and external packages into one isolated snapshot, remaps source World references to that
snapshot, and compares the Actor's direct serialized asset identities. It does not use the
WorldPartition-manager-derived `GetDataLayerInstances()` view as a persisted-state oracle.
