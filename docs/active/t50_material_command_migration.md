# T50 Material command authority

Current default methods:

```python
client.material.asset_create(...)
client.material.function_graph_apply(...)
client.material.graph_apply(...)
client.material_instance.asset_create(...)
client.material_instance.parameters_apply(...)
```

They are found by command_search and command_detail and mirrored by uai-cli
exact execution. The Material command owner delegates creation, graph
mutation, compile/settle, parameter application, persistence, and readback to
the common operation service and native UE material mechanics. It does not
duplicate graph or parameter business in the SDK.

Success requires the terminal native projection and independent persisted
readback. Prewrite failures leave no residue; unknown state is reconciled.
Cleanup is bounded to owned fixtures and independently verified.

