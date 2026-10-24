# T49 Animation Blueprint command authority

The default methods are:

```python
client.animation.anim_blueprint_layer_interface_asset_create(...)
client.content_graph.import_inline(...)
workspace = client.content_graph.export_workspace(...)
workspace.import_changes()
```

For unknown command names or parameter shapes, follow [public command discovery](../../../../UeAgentInterfaceCMD/docs/PYTHON_SDK_AND_CLI.md#command-discovery). No task-specific generated projection remains active.

Animation Blueprint graph mutation is an HJSON authoring operation. Its
internal operation kind remains available to the content graph import
mechanics, but is not a public command or named SDK method.

Create an Animation Blueprint at a missing target through HJSON `import_inline`;
edit an existing asset through workspace export/import. See
[the Animation Blueprint domain](t16_animation_blueprint_domain.md).
The former `animation.anim_blueprint.asset.create` identity is dormant and is
not a generated named SDK method. Layer-interface creation remains active.

Animation Blueprint authoring and layer-interface creation delegate
compile/settle, persistence, and readback to the common operation service and
native mechanics. They do not duplicate compile business in the SDK.
