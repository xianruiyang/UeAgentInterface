# T50 Material command authority

Current default methods:

```python
client.material_instance.asset_create(...)
```

For unknown command names or parameter shapes, follow [public command discovery](../../../../UeAgentInterfaceCMD/docs/PYTHON_SDK_AND_CLI.md#command-discovery). Material creation uses `asset.factory.candidates` followed by
`asset.object.create`. Material and material-instance authoring use
`content_graph.export_workspace` / `content_graph.import_workspace`; the old
typed material-create and instance-parameter apply identities remain internal
operation kinds only. Material-instance creation stays public until the common
factory path covers its native initialization.

Success requires the terminal native projection and independent persisted
readback. Prewrite failures leave no residue; unknown state is reconciled.
Cleanup is bounded to owned fixtures and independently verified.
