# T52 PCG command authority

The default public methods are:

```python
client.pcg.component_apply(...)
client.pcg.component_world_binding_apply(...)
client.pcg.data_asset_apply(...)
client.pcg.data_asset_create_from_level(...)
client.pcg.generated_spatial_data_read(...)
client.pcg.graph_apply(...)
client.pcg.graph_instance_apply(...)
client.pcg.graph_parameters_apply(...)
client.pcg.partition_spatial_state_read(...)
```

They are discovered through command_search and command_detail and mirrored by
uai-cli exact execution. The PCG command owner delegates graph/component/data
asset mutations, generation, persistence, readback, and cleanup to the common
services and native PCG mechanics. Queries retain their actual state layer.

Spatial contracts keep semantic_frame_id, unit, bounds convention,
basis_evidence, and conversion_chain explicit. The SDK contains no PCG or
spatial business. Terminal write success requires native settle and independent
persisted readback. Prewrite failure leaves no residue; unknown state is
reconciled. Cleanup is bounded and independently verified.

