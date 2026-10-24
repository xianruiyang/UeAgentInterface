# T52 PCG command authority

The default public methods are:

```python
client.pcg.data_asset_create_from_level(...)
client.pcg.generated_spatial_data_read(...)
client.pcg.partition_spatial_state_read(...)
```

For unknown command names or parameter shapes, follow [public command discovery](../../../../UeAgentInterfaceCMD/docs/PYTHON_SDK_AND_CLI.md#command-discovery). The PCG command owner delegates graph/component/data
asset mutations, generation, persistence, readback, and cleanup to the common
services and native PCG mechanics. Queries retain their actual state layer.
PCG graph topology and graph-parameter editing use
`content_graph.import_workspace`; their internal HJSON operation kinds are not
projected as separate public SDK commands.

PCG attribute/property selectors use the shared property codec as native text
leaves (`pcg_selector`, with the actual input/output `struct_path` retained).
In ordinary `properties.hjson`, edit the existing `value` directly, for example
`"$Position"`, `"$Transform.Translation"`, `"FenceHeight"`, `"@Last"` or `"@Source"`.
The same representation applies inside arrays and nested override descriptions.
Encoding uses native `ToString`; applying uses native `Update`, including empty
selectors and unchanged values. Native defaults and binding validation remain
PCG responsibilities. Protected selector storage fields are not authoring fields;
do not construct `Selection`, `AttributeName` or `ExtraNames` objects manually.

PCGDataAsset content editing uses the ordinary `pcg_data_asset` Reflection
workspace: export the asset, edit `data.source` and `data.sets`, then import.
Point fields use shared Details values and ordered item bindings. See
[PCGDataAsset workspace](../hjson-authoring/10_ReflectionAssetDomain.md#9-pcgdataasset-普通-workspace).
The old `pcg.data_asset.apply` identity remains dormant for compatibility;
it is not a generated named SDK method. `data_asset_create_from_level` remains
the independent native level-to-data conversion operation. Its `target_ref`
is a destination package path (`/Game/PCG/Points`), and `source_level_ref` is a
World object path (`/Game/Maps/Source.Source`). Ordinary workspace export and
later asset operations use the created object path (`/Game/PCG/Points.Points`).

Component graph binding and instance overrides use the existing Level component
workspace `pcg` state. `graph` is a shared asset reference, `parameters` is the
shared property-bag value, and `overrides` lists parameter names with local
overrides enabled. Clear that list to restore inheritance. `GenerationTrigger`
remains an ordinary component property. The old component, world-binding and
graph-instance apply identities are dormant compatibility routes.
Changing the graph while leaving exported parameter values unchanged inherits
the new parent values. Clearing `graph` clears its dependent parameter schema and
overrides through native SetGraph; old exported values do not recreate them.

## Explicit Editor generation

`editor.session` family `pcg_generation`, policy
`policy:pcg-generation:generate/v1`, runs native PCG generation for one exact
loaded component in the current Editor world. Obtain its `component_path` from
the spatial read below; prefix it with `pcg-component:`. Parameters are empty.

```python
started = client.editor.session(
    family_id="pcg_generation",
    subject_ref="pcg-component:" + component_path,
    policy_id="policy:pcg-generation:generate/v1",
    parameters={},
)
result = started.operation.finish()
```

Check the start and final results. This finite operation explicitly requests a
generation even for unchanged inputs. It preserves native graph caching and
Editor settings. `details.generation_started` and `generation_completed` refer
to this operation's native task and completion event; an existing `generated`
flag or persisted output is not evidence of a new generation. Inspect output
and geometry independently through the reads below. Completion confirms the
native generation lifecycle, not the content's visual correctness.

The component must be active, assigned a graph, placed outside preview/runtime
generation, and free of existing generation, cleanup or refresh work. The
operation rejects another owner's work and cancels only the task it started.
Cancellation does not promise rollback of partial generated content. Recovery,
cancellation and cleanup use the common Operation handle; successful output
survives release. Generation changes Editor memory; save the affected map and
resources through the existing Editor save entry when persistence is required.

Native Cleanup intentionally leaves the component ungenerated. Ordinary Level
imports retain UE's automatic-refresh eligibility and do not undo that stop.
Use this explicit policy to generate after Cleanup. `GenerateOnLoad` does not
mean that reopening a map in the Editor necessarily generates PCG content.

### Persistent cleanup

`world.pcg.cleanup` resolves one Actor and one PCG component with the common
scoped-object resolver. Its native resource inventory includes the original
component, registered partition locals, generated Actors and their nested PCG
components, and single/list managed components. Clearing the native ownership
array alone is insufficient: previously owned objects must also be absent.

The WorldDestructive runner journals the affected map and external packages,
including deleted external Actor packages. Independent persisted world readback
must confirm retained PCG components are ungenerated and their removed resources
are absent before reporting `committed`. The package checkpoint includes current
unsaved memory needed for world reload; unchanged associated packages preserve
their existing disk versions on successful publication. On failure, the runner
reloads the memory checkpoint, restores original disk artifacts and dirty flags,
and confirms the native resource inventory. A failed recovery remains
`state_unknown` with its original error and recovery diagnostics; Editor Undo is
not used as proof that PCG destruction was reversed. Cancelled in-flight native
generation is not restarted by recovery.

## Current native generation observations

The two spatial reads retain their public Python signatures:

```python
client.pcg.generated_spatial_data_read(target, {}, {})
client.pcg.partition_spatial_state_read(target, {}, {})
```

Contract version 2 reads an exact **loaded world, level, actor or PCG component**
returned by public readback. It does not load another world or generate/settle
content. Asset component templates are not actual generated world instances.
Internal native generation task IDs are excluded; generation status is exposed
as explicit booleans. `input` may select an `actor` and/or `component` by exact native name,
actor label, scoped discovery name or object path within that target. An explicit selector
must identify one object; ambiguous names return the matching object paths, never a first
match or an accidental union. Omit the selector to read the complete component collection.
An unmatched explicit selector is a located input error; an unfiltered scope without PCG
components is a valid empty observation.
`context: {}` uses that target's actual native scope; supplied `world` or `level`
must match it. No model-generated generation/revision identity is required.

The result `components` contains native component status plus either:

- `output`: `UPCGComponent::GetGeneratedGraphOutput()`, including the native
  local components mapped to a partitioned original component. This is the
  actual current collection, never a substitute PCGDataAsset or cached fixture.
- `partitions`: the subsystem's current component-to-partition mapping, with
  grid, bounds and local-component state. Nonpartitioned components have an
  empty array; querying never creates partition actors.

Generated output can be empty when the graph's Output node has no connection
even though mesh spawners created geometry. Read generated geometry through
`content_graph.items(..., fields=["geometry_components"])`; do not equate an
empty output collection with generation failure. Status exposes generating,
cleaning, generated and output count separately.

Point transforms retain **native PCG data coordinates**; point `bounds_min/max`
are **point-local bounds**. No implicit component transform or world-coordinate
conversion is applied. Partition fixed bounds are native world bounds in cm.
`max_points_per_data` defaults to `-1` (all points); `0` requests counts only,
and a nonnegative integer explicitly requests a sample. The returned point
count describes the full data set, not the sample size.

The existing public projection profile fields remain accepted;
`include_details` includes editable component properties. Defaults/provenance
flags do not synthesize values absent from this native observation.

The old RigSimulationSpatial generation-identity/DataAsset fixture route,
typed payloads and dedicated tests have exited. Query and generation ownership
remain separate: HJSON component authoring uses native generate/settle; these
queries observe its result. Write failures and cleanup retain the common
operation and recovery contracts.
