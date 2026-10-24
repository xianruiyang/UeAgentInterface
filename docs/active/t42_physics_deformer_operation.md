# T42 Physics/deformer operation authority

Physics/deformer commands retain their query or operation owners. Only active
commands are projected as named SDK methods. Use command search and usage to
obtain the current arguments; examples of current named methods are:

```python
client.animation.deformer_graph_asset_create(...)
client.animation.mesh_deformer_collection_asset_create(...)
client.deformer.evaluation_sample(...)
client.deformer.geometry_sample(...)
client.deformer.training_inputs_validate(...)
client.physics.simulation_probe(...)
```

The CLI accepts the command name and one parameter object; its generated
binding supplies machine identity. No task-specific projection is a formal entry.

`animation.deformer_graph.topology.apply` is a dormant compatibility identity,
not a named SDK method. Its existing node-name operation ensures a comment node;
that contract does not establish general Optimus candidate creation. Normal
asset editing belongs to the shared HJSON workspace commands. The former HJSON
adapter's arbitrary name-to-comment behavior does not prove general graph editing.

For an existing Deformer UpdateGraph, export with `content_graph.export_workspace`,
add a node to its ordinary `nodes` object using `candidate: {name: "Custom Compute Kernel"}`
for that native basic-node type, then apply with `content_graph.import_workspace`. Position is optional;
omitting it places the new node at the origin. Existing positions are exported as
`pos: {x: ..., y: ...}` on ordinary node entries and can be edited there. The importer resolves the real class,
preserves existing nodes, and completes shared persistence and final readback before
commit. No class path or hand-maintained machine manifest is required.

This basic-node path has been verified with Custom Compute Kernel and independent
disk reload/re-export. It does not establish complete native menu coverage or a
Deformer candidate-query endpoint; `animation_rig_graph.candidates` targets Control Rig.
New nodes may include `properties: [{property_path: "GroupSize", value: {fields: {X: 32, Y: 1, Z: 1}}}]`
in the same entry. The importer binds these descriptors to the newly created native
node and shares the existing recursive property apply and persisted-intent checks.
After refresh, edit the exported property file; no creation-only property fragment remains.
Editable root and existing-node properties use the shared reflected-property codec
and apply path. Custom Compute Kernel `GroupSize` has passed ordinary property
editing, independent disk reload, and re-export with all other persistent state
preserved. Its recursive value is `value: {fields: {X: 32, Y: 1, Z: 1}}`.
Adding an entry to the ordinary `InputBindingArray` value has also passed native
input-Pin reconstruction, shader-declaration update, independent reload and
re-export. Copy an existing exported binding entry and edit its name; the shared
property path restores the recursive codec representation internally.
Ordinary `*.interfaces.hjson` files expose graph-local Pin aliases, including
nested input Pins. Use these endpoints in the graph's `connections` array.
Connection creation has passed public import, native endpoint checks, independent
disk reload and re-export. Adding an input binding while retaining an existing
connection has also passed Pin reconstruction and independent persisted readback.
The same import can add a binding and connect its new Pin: retain the exported
input alias's group prefix and use the new binding name. Both the retained and new
connections have passed independent reload and re-export. The root `Mesh` preview
reference can be cleared with `value: null` in its ordinary property entry. Clearing
an existing reference together with a `GroupSize` edit has passed compile, independent
reload and re-export while preserving other persistent state. Other creation
categories remain incomplete.
Native node deletion has passed both pre-release
staging failure and post-reload final-readback rejection: shared checkpoint recovery
restores the dirty in-memory graph and the original disk package separately.

The owner delegates asset creation, graph/topology changes, physics mutation,
sampling, validation, persistence, and readback to the shared query/operation
services and native UE mechanics. It does not duplicate those mechanics in the
SDK. Spatial and geometry values remain explicit through semantic_frame_id,
unit, basis evidence, and conversion chain where applicable.

Write success requires the terminal operation projection plus an independent
persisted readback. Read-only probes retain their actual state layer. Invalid
targets, schemas, topology, frames, or stale revisions fail before mutation;
unknown post-write state is reconciled and is not blindly replayed. Cleanup is
bounded to owned fixtures and its absence is independently verified.
