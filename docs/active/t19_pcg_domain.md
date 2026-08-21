# T19 PCG HJSON domain

`pcg-domain-provider` is the sole `content_graph.*` domain provider for
`/Script/PCG.PCGGraph`. The public SDK calls are
`client.pcg.node_candidates(...)` and
`client.content_graph.import_workspace(...)`; the schema is
`uai.pcg.edit/1`. Inspect the live contracts through
`client.command_search(...)` and `client.command_detail(...)`; `uai-cli exec`
forwards the same exact generated binding and is the CLI ingress.

The shared source loader owns inline, single-file, and directory carriers. T19
consumes only the normalized `source.kind=resolved` document set and does not
accept legacy top-level `carrier` or `hjson_document` fields.

T13 remains the sole common recursive initialization authority. T19 registers
only PCG root, node, endpoint, and parameter initialization contracts and
consumes that shared seam; it does not create another recursive dispatcher.

## Root lifecycle

A new PCG Graph uses `expected_revision: "missing"` and a root `create` object.
The public target owns the final package path and asset name; conflicting
declarations fail before write. Root creation delegates to the existing
`pcg.graph.asset.create` owner and cannot be combined with an edit in the same
import. Create the root first, use its independent readback revision, then issue
the edit as a second explicit public command.

An existing graph uses the exact current `content_graph` revision and one root
`operation`:

- `pcg.graph.apply` for node, pin, edge, ordering, and supported graph edits;
- `pcg.graph_parameters.apply` for graph parameter edits.

The nested `target_ref` must match the public request target. T19 forwards one
normalized request to the existing PCG mechanics owner; it does not copy graph
planning, UE node construction, compile, save, reload, or cleanup behavior.

## Candidate-to-authoring chain

`pcg.node.candidates` returns bounded exact non-abstract `UPCGSettings` class
candidates. Copy the full candidate object unchanged into the creation edit's
`candidate` field. The domain validates its identity, contracts, supported
operation, and `binding_revision` against the current target, then supplies the
exact settings class to the native PCG edit. Candidate conflicts and stale
bindings are prewrite failures.

## Lifecycle and readback

Both root creation and mutation must return a real mutation receipt, settled
compile state, saved package, unload/reload receipt, and independent persisted
`ContentGraphRead` snapshot. Unsupported schema/root/operation combinations,
stale revisions, target mismatches, invalid candidates, and lifecycle failures
fail explicitly without a legacy fallback. `content_graph.export_workspace`
remains explicitly unavailable until a complete round-trip export contract is
registered.
