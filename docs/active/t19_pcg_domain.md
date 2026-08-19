# T19 PCG domain

The PCG HJSON domain is a thin adapter over the shared `content_graph.*`
workspace contract and the exact T52 PCG command owners. It does not own HJSON
parsing policy, recursive traversal, default resolution, reference resolution,
cleanup, persistence, or a parallel PCG mutation implementation.

The public SDK calls are `client.pcg.node_candidates(...)` and
`client.content_graph.import_workspace(...)`. Use
`client.command_search(...)` and `client.command_detail(...)` to discover and
inspect their live contracts; `uai-cli` exposes the same discovery path. No
task-specific helper or tutorial-owned registry is authoritative. T13 is the
sole common recursive initialization authority; T19 only contributes PCG
initialization contracts and consumes that seam. T13 is also the sole carrier
loader and directory-recursion authority: T19 consumes the normalized
`source.kind=resolved` document set and selects exactly its declared
`entry_file`; it does not accept legacy top-level `carrier` or
`hjson_document` fields.

## Public surface

- `pcg.node.candidates` resolves bounded, exact non-abstract `UPCGSettings`
  class identities for node creation.
- The nine shared `content_graph.*` commands read or import a PCG Graph through
  the common HJSON workspace entry.
- The domain schema is `uai.pcg.edit/1`.
- The root initialization contract is `uai.init.pcg_graph.root/1`.
- Node, endpoint, and parameter initialization contracts are respectively
  `uai.init.pcg.node/1`, `uai.init.pcg.endpoint/1`, and
  `uai.init.pcg.parameter/1`.

## Import contract

An import targets an existing persisted PCG Graph and must
carry its current `content_graph` revision. The HJSON root contains exactly one
`operation` with an exact T52 command ID and params. Graph roots accept
`pcg.graph.apply` and `pcg.graph_parameters.apply`. The nested `target_ref`
must identify the same target as the public import request.

No root-create command currently exists in the active PCG command authority.
Therefore `expected_revision: "missing"` is rejected before any write with
`pcg_domain_root_create_not_registered`; no compatibility factory is added.
`content_graph.export_workspace` is likewise explicitly unavailable until the
shared export authority supplies that contract.

## Failure and readback

Unsupported root types, schemas, operations, missing documents, stale
revisions, mismatched targets, unavailable native mechanics, and unsupported
create/export requests fail explicitly. Successful imports delegate once to the
exact PCG command owner and then independently capture the persisted graph
through the common content-graph reader. Candidate discovery is read-only and
creates no package, node, file, or external state.
