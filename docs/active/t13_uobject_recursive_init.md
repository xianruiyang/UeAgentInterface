# T13 UObject/DataAsset recursive initialization

## Public owner and routes

The active public owner is
`content-graph-workspace-owner`. It owns exactly these
contracts:

- `content_graph.export_workspace`
- `content_graph.import_workspace`

The process active composition freezes the contracts once, then constructs one
exact handler per route against the same final registry. The module ingress
grants the authenticated local principal the route-level `uai:export` and
`uai:operate` scopes. Export requires `uai:export`, and import requires
`uai:operate`.

The seven read-only `content_graph.*` facets belong to `query-command-owner`
and the shared ContentGraphRead Query gateway. This workspace owner is not a
query fallback or a second result serializer.

The workspace routes use the process-frozen HJSON domain-provider registry
before domain execution. The registered provider identity is
`uobject-dataasset-domain-provider`. The public router supplies the command,
schema, exact target, root type, structural role, and canonical parameters; it
does not select a domain initialization contract. The provider uniquely
resolves `uai.init.uobject_dataasset.root/1` for the DataAsset root and
`uai.init.uobject_dataasset.owned/1` for recursively owned objects. Unknown or
ambiguous schema/type/role ownership fails before domain execution. Later
domains contribute providers to this same registry instead of adding a central
command branch or a second `content_graph.*` owner.

## Recursive import contract

`content_graph.import_workspace` accepts:

- `target`: exact `/Game/package.object` identity;
- `source`: exactly one discriminated source object:
  - `{"kind":"inline","document":<HJSON>}`;
  - `{"kind":"single_file","path":<absolute .hjson path>}`;
  - `{"kind":"directory","path":<absolute workspace root>}`.

The public SDK receives only `target` and `source`. Directory edits
recover the target and concurrency baseline from the hidden workspace manifest; inline and
single-file sources are create-only and use the server-side missing expectation.

The legacy top-level `carrier + hjson_document` shape is rejected rather than
treated as an overload. Legacy directory sources contain `.uai/workspace.hjson` with
`workspace_schema=uai.content_graph.workspace/1`, `format_version=1`, the exact
`target_identity`, `base_revision`, `entry_file`, a `files[]` inventory of
relative `.hjson` paths and SHA-256 `content_hash` values, and a
`references[]` closure. Before provider routing, the shared source loader
normalizes absolute paths, rejects traversal and reparse points, reads every
declared file, rejects undeclared HJSON, verifies target/digests/references,
and confirms that the source version did not change. Inline, single-file, and
directory then become one immutable internal resolved source; providers never
reopen caller paths. Semantic directory workspaces instead delegate document and
new `content_ref` assembly to the common semantic resolver; unrelated files are
not rejected as undeclared semantic content. See the T09 import contract.

The document root declares the DataAsset type, explicit values, recursively
owned objects, optional ownership overrides, and cross-object references. Types
are exact `/Script/...` identities. This provider supports the registered
`UeAgentRecursiveDataAsset` root and `UeAgentRecursiveOwnedObject` children;
it does not promise arbitrary DataAsset creation. The production owner parses through the
common HJSON parser and gives the real node classes, explicit values, ownership
and reference edges to the common import-initialization planner before any write.
It does not compile an empty twelve-unit model or wrap an unused Unified model.

Execution creates the root through AssetTools and `UDataAssetFactory`. Owned
objects use `NewObject` with their real parent as `Outer`; native constructors
create default subobjects before explicit values are applied. References are
bound after creation, allowing forward/shared references and reference cycles
while rejecting ownership cycles.
The native owned class is `EditInlineNew`; ownership is declared explicitly on
`OwnedObjects`, `Children` and default-subobject properties. `Peer` and `Shared`
are ordinary object references, so Reflection does not expand them as owned
subtrees.

The root factory, native names and `Outer` hierarchy, recursively owned object
creation, and post-creation reference binding remain domain mechanics because
the shared initialization planner only plans ordering and edges. Scalar and
container values use the common UObject property codec/apply path. The domain
maps the document fields `label`, `explicit_number`, `numbers`, and
`named_numbers` within each node's `values` object to their native properties; omitted fields are not synthesized
and therefore retain constructor defaults.

Before creating the root, the provider prepares every node's scalar/container
values through the shared ObjectApply API against the registered native class
defaults. This checks property eligibility, value types and numeric bounds
without applying a plan to a default object. The same preparation is repeated
against each real object to bind its addresses before assignment and verification
inside the existing graph lifecycle. It does not commit separate per-object
editor undo transactions before the graph's final acceptance.

The common operation lifecycle still performs pre-execute fencing, mutation,
settle, package save, unload/reload, independent readback, and confirmed
rollback internally. A successful public `content_graph.import_workspace`
response is intentionally minimal: `{status: "succeeded"}`, with a
`workspace_refresh` outcome for semantic directory imports. A refreshed directory
contains the standard export and current baseline. A failed refresh returns
`content_graph_import_applied_workspace_refresh_failed`, explicitly reporting
that the asset was applied and must not be imported again as a retry.
Callers use the formal query/export surface for any additional
post-write state they actually need. Provider identity, revisions, mutation
metrics, persistence receipts, and independent readback are internal evidence
or separate readback concerns, not default model-facing result fields.
Independent verification compares the complete planned object inventory,
native names and `Outer` relationships, reference endpoints, explicit values,
containers, and complete native default-subobject state with a fresh unload/reload
capture. The expectation is built from the immutable request, accepted ownership
plan, reference edges and native class defaults before mutation. The common success projection is also built inside that
recoverable verification stage, so a projection failure uses the existing
rollback and cannot leave a saved create behind.

## Editable workspace and independent readback

For the registered native recursive DataAsset, the same domain provider delegates
default export and semantic directory edits to the shared Reflection workspace
profile `recursive_dataasset`. This profile does not enable generic asset creation.
Visible Category/type/value rows expose root and owned descendant properties;
existing owned occurrences and references retain their workspace identities.
Owned collection edits use the common ObjectApply path and native candidate
eligibility. The absent-root recursive document remains the creation contract.

Workspace publication uses the common verified Reflection export. Its default public response is the same minimal
`{status: "succeeded"}`; workspace paths, hashes, publisher data and semantic
verification remain hidden machine evidence.

## Public SDK and CLI

The final manifest projects both workspace contracts into the default
`content_graph` namespace. Callers import only `uai`, use `uai.connect_project()` for the local project,
and use `client.content_graph.import_inline(...)` or
`client.content_graph.export_workspace(...)`. Generic
`client.execute(...)`, exact `client.execute_exact(...)`, and `ueagentinterfacecmd exec`
reuse the same generated catalog, Client, Protocol, and Transport. No
T13-specific Python module, provider selector, HJSON parser, command-ID table,
contract-hash table, lifecycle controller, or UE mutation path remains in the
client.

```python
result = client.content_graph.import_inline(
    "/Game/StrictV2/T13/DA_Recursive.DA_Recursive",
    document,
)
```

```python
workspace = client.content_graph.export_workspace(
    "/Game/StrictV2/T13/DA_Recursive.DA_Recursive",
    name="dataasset",
)
# Edit the visible Category/type/value rows below workspace.path.
result = workspace.import_changes()
```

The raw wire path is SDK/server-owned machine state and is not a model-facing
argument or result field.

Read the refreshed workspace after directory import; export separately only when an independent observation or refresh recovery is needed. Keep exported identity
and reference selectors unchanged unless the corresponding structural edit is
intended. Machine-owned workspace metadata is not an authoring surface.

## Failure and cleanup

Unknown types, malformed or unsupported fields, stale/existing targets,
ownership cycles, resource excess, missing authority, and lifecycle failures
are typed business failures. Validation and planning failures occur before
write. Failed absent-root initialization removes its newly created package.
Failed workspace edits use the shared Reflection checkpoint to restore the
original in-memory state, persisted bytes and dirty state. Final readback occurs
inside this recoverable operation. No route
falls back to a legacy provider or reports partial success as success.

Directory-specific prewrite failures include missing or malformed machine
manifest, target mismatch, invalid/escaping paths, reparse points, missing or
undeclared documents, digest mismatch, unresolved declared references, and a
source version change during load.
