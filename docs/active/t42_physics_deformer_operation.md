# T42 Physics/deformer operation authority

All 15 commands are owned by the current physics/deformer query or operation
owner and are exposed only through the default SDK catalog and exact CLI.
Use client.command_search(...) and client.command_detail(...) before invoking
one of the named methods:

```python
client.animation.deformer_graph_asset_create(...)
client.animation.deformer_graph_topology_apply(...)
client.animation.deformer_source_library_asset_create(...)
client.animation.deformer_source_library_bindings_apply(...)
client.animation.mesh_deformer_collection_asset_create(...)
client.animation.mesh_deformer_collection_entries_apply(...)
client.animation.ml_deformer_asset_create(...)
client.animation.ml_deformer_configuration_apply(...)
client.deformer.evaluation_sample(...)
client.deformer.geometry_sample(...)
client.deformer.training_inputs_validate(...)
client.physics.asset_structure_replace(...)
client.physics.constraint_update(...)
client.physics.material_update(...)
client.physics.simulation_probe(...)
```

The CLI projection is uai-cli with the command ID, current contract hash, and
one JSON parameter object. No task-specific generated projection is a formal
entry.

The owner delegates asset creation, graph/topology changes, physics mutation,
sampling, validation, persistence, and readback to the shared query/operation
services and native UE mechanics. It does not duplicate those mechanics in the
SDK. Spatial and geometry values remain explicit through semantic_frame_id,
unit, basis evidence, and conversion chain where applicable.

Write success requires the terminal operation projection plus an independent
persisted readback. Read-only probes retain their actual state layer. Invalid
targets, schemas, topology, frames, or stale revisions fail before mutation;
unknown post-write state is reconciled and is not blindly replayed. Cleanup is
bounded to owned fixtures and its absence is independently verified.

