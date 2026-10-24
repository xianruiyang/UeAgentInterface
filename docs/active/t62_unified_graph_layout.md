# T62 Unified Graph Layout

`graph.layout.apply` is the only public persistent graph-layout command. Discover
its name with `client.query.command_search(...)`, inspect its minimal call with
`client.query.command_usage(...)`, and then use the generated SDK:

```python
result = client.graph.layout_apply(
    targets=[
        {
            "graph_identity": "/Game/UI/BP_Menu.BP_Menu:EventGraph",
        }
    ],
    strategy="hierarchical_left_to_right",
    parameters={"horizontal_spacing": 320, "vertical_spacing": 160},
)
```

The same request may contain one or more exact graph identities only when they
belong to the same owning Blueprint asset and package. The server derives the
provider from the real owning asset type, freezes the current owning-asset state
after resolution, and rechecks it immediately before the first write; callers
never pass provider names, revisions, hashes, or authorization identities. It
rejects unknown, duplicate, cross-package, unsupported, changed, or unmeasurable
targets before the first write. It measures node and pin geometry in an
offscreen graph editor, applies the shared topology algorithm, compiles and
saves the one package, reloads it, and returns only a short per-graph layout
summary. Exact persisted state is read again through the normal graph query when
needed.

The owning-asset revision comes from the shared native Blueprint capture,
including the existing authoring snapshot's graph signatures, node properties
and editable pin defaults; it does not depend on domain-specific summary fields.
Revision capture failure is distinct from a successful read that detects changed
state. Failures use the common structured diagnostic carrier: target and stage,
parameter field and correction where applicable, and native compiler errors for
compile failures. Private native details are not copied into public output.

Omitted spacing, padding and origin fields keep their defaults. Explicit values
that cannot be read as numbers report the offending `parameters.<field>` instead
of silently using a default. The existing finite/positive spacing and
non-negative padding rules are unchanged.

The current verified provider families are Blueprint, Widget Blueprint, and
Animation Blueprint. Material, Niagara, PCG, AI, Audio, Control Rig, and
cross-package atomic batches remain unsupported until their own verticals add
real provider mechanics; the command does not advertise guessed routing for
them. Domain-specific layout aliases and the old Viewport-owned route are not
public entries. `ueagentinterfacecmd` uses the same exact command ID and parameter object.
