# T62 Unified Graph Layout

`graph.layout.apply` is the only public persistent graph-layout command. Discover
its current hash and parameter contract with `client.query.command_search(...)`
and `client.query.command_detail(...)`, then call:

```python
result = client.graph.layout_apply(
    targets=[
        {
            "graph_identity": "/Game/UI/BP_Menu.BP_Menu:EventGraph",
            "expected_revision": "graph-layout:0123abcd",
        }
    ],
    strategy="hierarchical_left_to_right",
    parameters={"horizontal_spacing": 320, "vertical_spacing": 160},
    authorization_id="auth:layout:menu",
)
```

The same request may contain one or more exact graph identities, including
different supported graph domains. The server derives the provider from each
loaded graph type; callers never pass provider names. It rejects unknown,
duplicate, unsupported, or stale targets before the first write, forms one
impact plan, applies the layout, saves every target package, and returns node
positions and revisions from an independent readback.

Blueprint, Animation Blueprint, Material, Niagara, PCG, AI, Audio, and Control
Rig are provider families, not public command namespaces. Domain-specific
layout aliases and the old Viewport-owned layout route are retired. `uai-cli`
uses the same exact command ID, contract hash, and one JSON parameter object.
