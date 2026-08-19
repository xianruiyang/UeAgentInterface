# T13 UObject/DataAsset recursive initialization

## Public owner and routes

The active public owner is
`uobject-dataasset-recursive-initialization-owner`. It owns exactly these
contracts:

- `content_graph.compound`
- `content_graph.diagnostics`
- `content_graph.export_workspace`
- `content_graph.import_workspace`
- `content_graph.items`
- `content_graph.properties`
- `content_graph.relations`
- `content_graph.structure`
- `content_graph.summary`

The process active composition freezes the contracts once, then constructs one
exact handler per route against the same final registry. The module ingress
grants the authenticated local principal the route-level `uai:query`,
`uai:export`, and `uai:operate` scopes. Query routes require `uai:query`, export
requires `uai:export`, and import requires `uai:operate`.

The former ReadExport query routes and TimelineSpatial HJSON import route expose
no active command for these identities. They are not fallbacks.

All nine routes enter the process-frozen HJSON domain-provider registry before
domain execution. The registered provider identity is
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
  - `{"kind":"directory","path":<absolute workspace root>}`;
- `expected_revision`: `absent` for the calibrated create operation;
- `authorization_id`: authorization bound to the exact impact target;
- `limits`: optional recursive object-node and work-unit bounds.

The legacy top-level `carrier + hjson_document` shape is rejected rather than
treated as an overload. Directory sources contain `.uai/workspace.hjson` with
`workspace_schema=uai.content_graph.workspace/1`, `format_version=1`, the exact
`target_identity`, `base_revision`, `entry_file`, a `files[]` inventory of
relative `.hjson` paths and SHA-256 `content_hash` values, and a
`references[]` closure. Before provider routing, the shared source loader
normalizes absolute paths, rejects traversal and reparse points, reads every
declared file, rejects undeclared HJSON, verifies target/digests/references,
and confirms that the source version did not change. Inline, single-file, and
directory then become one immutable internal resolved source; providers never
reopen caller paths.

The document root declares the DataAsset type, explicit values, recursively
owned objects, optional ownership overrides, and cross-object references. Types
are exact `/Script/...` identities. The production owner parses through the
common HJSON parser, builds the unified semantic model, and uses the common
import-initialization planner before any write.

Execution creates the root through AssetTools and `UDataAssetFactory`. Owned
objects use `NewObject` with their real parent as `Outer`; native constructors
create default subobjects before explicit values are applied. References are
bound after creation, allowing forward/shared references and reference cycles
while rejecting ownership cycles.

The common operation lifecycle performs pre-execute fencing, mutation, settle,
package save, unload/reload, independent readback, and confirmed rollback. A
successful response includes `terminal_status`, `mutation_started`,
`persistence_receipt_id`, `plan_fingerprint`, `phase_history`, and
`independent_readback`.

## Read and export

The seven read routes resolve the persisted DataAsset and return `read_only`,
the real `mechanics_command_id`, and a `query_result` built from the UObject
graph. The readback includes type, Outer-derived ownership counts, native
defaults, explicit values, containers, shared-reference identity, and legal
reference-cycle preservation.

`content_graph.export_workspace` uses the verified-export service. Publication
occurs only after a staged `asset.hjson` rebuild produces the same semantic
signature. The response includes `status=published`, workspace and entry paths,
plan/inventory hashes, and `semantic_verification`.

## Public SDK and CLI

The final manifest projects all nine contracts into the default
`content_graph` namespace. Callers import only `uai`, connect from one TOML,
and use `client.content_graph.compound(...)`,
`client.content_graph.import_workspace(...)`, or
`client.content_graph.export_workspace(...)`. Generic
`client.execute(...)`, exact `client.execute_exact(...)`, and `uai-cli exec`
reuse the same generated catalog, Client, Protocol, and Transport. No
T13-specific Python module, provider selector, HJSON parser, command-ID table,
contract-hash table, lifecycle controller, or UE mutation path remains in the
client.

```python
result = client.content_graph.import_workspace(
    "/Game/StrictV2/T13/DA_Recursive.DA_Recursive",
    {"kind": "inline", "document": document},
    expected_revision="absent",
    authorization_id="auth:t13:approved-impact",
)
```

```text
uai-cli --connection connection.toml exec content_graph.summary --params-json {"target":"/Game/StrictV2/T13/DA_Recursive.DA_Recursive"}
```

## Failure and cleanup

Unknown types, malformed or unsupported fields, stale/existing targets,
ownership cycles, resource excess, missing authority, and lifecycle failures
are typed business failures. Validation and planning failures occur before
write. A failure after mutation starts reaches a truthful rolled-back terminal;
the owned asset package is removed and rollback must be confirmed. No route
falls back to a legacy provider or reports partial success as success.

Directory-specific prewrite failures include missing or malformed machine
manifest, target mismatch, invalid/escaping paths, reparse points, missing or
undeclared documents, digest mismatch, unresolved declared references, and a
source version change during load.
