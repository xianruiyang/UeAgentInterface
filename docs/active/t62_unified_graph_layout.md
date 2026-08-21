# T62 Unified Graph Layout

`graph.layout.apply` is the only public persistent graph-layout command. Discover
its current hash and parameter contract with `client.query.command_search(...)`
and `client.query.command_detail(...)`, then call:

```python
result = client.graph.layout_apply(
    targets=[
        {
            "graph_identity": "/Game/UI/BP_Menu.BP_Menu:EventGraph",
            "expected_revision": "content-graph:0123abcd",
        }
    ],
    strategy="hierarchical_left_to_right",
    parameters={"horizontal_spacing": 320, "vertical_spacing": 160},
    authorization_id="auth:layout:menu",
)
```

The same request may contain one or more exact graph identities only when they
belong to the same owning Blueprint asset and package. Read that asset first
through `content_graph.*` and pass its public `revision` for every target. The
server derives the provider from the real owning asset type; callers never pass
provider names. It rejects unknown, duplicate, cross-package, unsupported,
stale, or unmeasurable targets before the first write. It measures node and pin
geometry in an offscreen graph editor, applies the shared topology algorithm,
compiles and saves the one package, reloads it, and returns persisted node
positions plus the new public revision from an independent readback.

The current verified provider families are Blueprint, Widget Blueprint, and
Animation Blueprint. Material, Niagara, PCG, AI, Audio, Control Rig, and
cross-package atomic batches remain unsupported until their own verticals add
real provider mechanics; the command does not advertise guessed routing for
them. Domain-specific layout aliases and the old Viewport-owned route are not
public entries. `uai-cli` uses the same exact command ID and parameter object.
