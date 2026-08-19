# T40 TextureGraph Operation Authority

The active `texture-graph-operation` contribution owns exactly seven current
TextureGraph command identities. The separate historical inventory assigns
seven pre-refactor identities to `T40-TEXTURE-GRAPH-OPERATION`; their distinct
successors are recorded in
`docs/active/successors/t40_texture_graph_operation.json`. Every frozen current
contract reports
`texture-graph-operation-owner`; no namespace or catch-all fallback adds
commands.

The four mutation commands and `texture_graph.artifact.bake` enter the common
Operation lifecycle through five request-scoped factories. The public handler
and typed capability contribution consume the same factory object for each
identity. Read-only `texture_graph.artifact.capture` and
`texture_graph.runtime.probe` enter the common Query lifecycle. These lanes
reuse the single native GraphCompound TextureGraph mechanics boundary. The
former AudioMediaGraph registry stage is an internal mechanics dependency only,
not a second public route, registry, handler, contract, test or documentation
authority.

Mutation and bake success require exact schema and target binding, native
application, settle, required persistence and independent readback. Bake
renders every selected output, creates the requested Texture2D targets as one
authorized impact set, and compensates the full batch on failure. Artifact
capture accepts
only the exact settled revision and returns native output identities without
publishing a second artifact authority. Runtime probe performs a real render,
returns pixel statistics and expected-stat comparisons, and never persists.
Prewrite failures leave no residue;
unknown state requires reconciliation rather than replay.

## Public SDK and CLI

The final manifest projects all seven identities into the default
`texture_graph` namespace. Callers import only `uai`, connect from one complete
Connection TOML and call methods such as
`client.texture_graph.artifact_capture(...)`,
`client.texture_graph.artifact_bake(...)`,
`client.texture_graph.graph_node_mutate(...)`, and
`client.texture_graph.runtime_probe(...)`. Generic `client.execute(...)`,
exact `client.execute_exact(...)`, and `uai-cli exec` reuse the same generated
catalog, Client, Protocol and Transport. No T40-specific Python module remains
authoritative.

```text
uai-cli --connection connection.toml exec texture_graph.artifact.capture --params-json {"expected_settled_revision":"package-md5:<current-settled-revision>","outputs":["Output"],"target_ref":"/Game/TextureGraphs/TG_Example"}
```

Use `client.query.command_search("capture texture graph artifact", limit=5)` and
then `client.query.command_detail(command_id, contract_hash)` to obtain the exact
request schema, named SDK/CLI forms, owner, failures and tutorial references.
The settled revision is target state, not a fixed tutorial literal; read it from
the preceding successful mutation/settle response and never guess it.
