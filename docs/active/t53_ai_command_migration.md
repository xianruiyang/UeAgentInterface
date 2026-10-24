# T53 AI command authority

Current default methods:

```python
client.ai.behavior_tree_asset_create(...)
client.ai.blackboard_asset_create(...)
client.ai.eqs_asset_create(...)
client.ai.smart_object_definition_asset_create(...)
client.ai.state_tree_asset_create(...)
```

For unknown command names or parameter shapes, follow [public command discovery](../../../../UeAgentInterfaceCMD/docs/PYTHON_SDK_AND_CLI.md#command-discovery). AI structure mutation uses the public
`content_graph.export_workspace` / `content_graph.import_workspace` HJSON
lifecycle; typed graph/schema/hierarchy/definition apply IDs remain internal
operation kinds and are not duplicate public commands. The AI owner delegates
compile/settle, persistence, readback, and cleanup to the common operation
service and native UE AI mechanics. It does not duplicate domain business in
the SDK.

Terminal success requires applicable native settle and independent persisted
readback. Prewrite failures leave no residue; unknown state is reconciled.
Cleanup is bounded to owned fixtures and independently verified.
