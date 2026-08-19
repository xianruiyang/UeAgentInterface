# T53 AI command authority

Current default methods:

```python
client.ai.behavior_tree_asset_create(...)
client.ai.behavior_tree_graph_apply(...)
client.ai.blackboard_asset_create(...)
client.ai.blackboard_schema_apply(...)
client.ai.eqs_asset_create(...)
client.ai.eqs_graph_apply(...)
client.ai.smart_object_definition_apply(...)
client.ai.smart_object_definition_asset_create(...)
client.ai.state_tree_asset_create(...)
client.ai.state_tree_hierarchy_apply(...)
```

They are found through command_search and command_detail and mirrored by
uai-cli exact execution. The AI command owner delegates creation, schema,
graph/hierarchy/definition mutation, compile/settle, persistence, readback, and
cleanup to the common operation service and native UE AI mechanics. It does not
duplicate domain business in the SDK.

Terminal success requires applicable native settle and independent persisted
readback. Prewrite failures leave no residue; unknown state is reconciled.
Cleanup is bounded to owned fixtures and independently verified.

