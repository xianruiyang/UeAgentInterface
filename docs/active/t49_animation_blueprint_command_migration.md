# T49 Animation Blueprint command authority

The default methods are:

```python
client.animation.anim_blueprint_asset_create(...)
client.animation.anim_blueprint_graph_apply(...)
client.animation.anim_blueprint_layer_interface_asset_create(...)
```

They are discovered through command_search and command_detail and mirrored by
uai-cli exact execution. No task-specific generated projection remains active.

The Animation Blueprint command owner delegates creation, graph mutation,
compile/settle, persistence, and readback to the common operation service and
native mechanics. It does not duplicate graph or compile business in the SDK.
Terminal success requires compile/settle plus independent persisted readback.
Invalid or stale inputs fail before mutation; unknown state is reconciled.
Cleanup is bounded to owned fixtures and independently verified.

