# T40 TextureGraph Operation Authority

The active `texture-graph-operation` contribution owns the current TextureGraph
command identities. Every current contract reports
`texture-graph-operation-owner`; no namespace or catch-all fallback adds
commands.

The three typed node/link/parameter mutation identities are dormant compatibility
commands. Normal graph editing uses the public HJSON workspace flow below;
their compatibility registration does not make them generated named SDK methods.

The four typed mutation/settle commands and `texture_graph.artifact.bake` enter the common
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
capture resolves the current settled asset on the server and returns the
selected output observations without publishing a second artifact authority.
Runtime probe performs a real render,
returns pixel statistics and expected-stat comparisons, and never persists.
Prewrite failures leave no residue;
unknown state requires reconciliation rather than replay.

## Public SDK and CLI

Existing TextureGraph assets also use the shared HJSON workspace commands:
`content_graph.export_workspace` and `content_graph.import_workspace`. Export
the asset first, edit its readable graph documents, then import the same
workspace directory. Native scalar, integer, Boolean, string, color and vector
inputs expose their compact value, including inputs that are not graph parameters.
Input parameters also expose their alias when no compact native value is available;
connections use the exported node and pin names. Keep the hidden machine
mapping unchanged. Read the refreshed workspace after import for the current graph
and interfaces before further edits.
If an older export reports `semantic_workspace_graph_pin_capture_reexport_required`,
export to a new directory and transfer the intended ordinary HJSON edits. That
old machine baseline lost the original pin selectors; do not reconstruct them from aliases
or overwrite the directory containing unapplied edits.

To add a known expression, add a keyed node with its readable `name`, for
example `AddedGain: {name: "Scalar"}`. Optional input values and connections may
be supplied in the same import. Creation resolves the readable expression name
and binds the resulting native node for subsequent parameter and connection
changes. Unknown or ambiguous names fail before mutation. This path does not
provide a complete native-menu candidate search or semantic `select` support.
The former `uai.texture_graph.edit/1` operation wrapper is retired; export a
current workspace instead of wrapping typed command IDs and parameters in HJSON.

Editable expression fields use the shared reflected property surface in each
node's `properties.hjson`, except values already represented by compact inputs.
This includes `TG_Setting`, input references such as Texture `Source`, and
reflected input structures such as `OutputSettings.Settings`. Edit the exported `categories`
`type`/`value` rows; enum names and legal choices come from the workspace's
shared definitions. For example, an IfThenElse node exposes
`NoCategory.Operator: {type: "EIfThenElseOperator", value: "LT"}`. Import follows
the captured node identity through the same current binding used by nodes and
connections. If the graph edit replaces the expression, settings are prepared
against the replacement after native creation. ObjectApply validates and applies
the original descriptors and sends the native property notification so the
setting Pin stays synchronized.
A value has one writable projection. Compact native inputs retain their
alias/value surface; other reflected fields use the same recursive codec and
property application as settings. An input containing only `alias` edits that
alias and preserves its value. Native outputs/private data are not editable
inputs, and live GPU values are not converted into invented asset references.
Integer inputs preserve their native width through export, import and readback;
fractional or out-of-range numbers are diagnosed at the input instead of truncated.

Setting edits join the same native transaction as graph edits. Invalid enum
values fail before source mutation. Native execution or persistence failures use
the existing recovery owner. A difference or incomplete final observation is a
warning; it does not replay or roll back an otherwise successful native change.
Direct coverage includes IfThenElse settings and identity, Source references,
OutputSettings structure edits, alias-only changes, and exact ArrayGrid integer
inputs through public import and persistent reload. Other expressions and
interface-changing settings have not been exhaustively validated.

Workspace updates observe the persisted native result before commit. Default
committed publication consumes the actual before/after facts; missing observation
keeps synchronization pending without advancing its watermark or repeating the
native operation. Explicit incremental/full exports remain available. Typed
compatibility operations retain their own lifecycle contract.


For a missing target, the same public import accepts an inline HJSON source
with the shared schema and a readable root factory candidate:

```hjson
{
  schema: uai.authoring.workspace/2
  root: {
    candidate: { name: "Texture Graph" }
    graph: {
      nodes: {
        Gain: {
          candidate: { name: "Scalar" }
          inputs: { Scalar: { alias: "Gain", value: 0.25 } }
        }
      }
      connections: []
    }
  }
}
```

Pass this text through `source: {kind: "inline", document: text}` and supply
the asset path as `target`. The server resolves the existing factory catalog
and initialization contract; no factory/class path or revision is required.
Native factory defaults, including the Output node, are preserved. The root
and requested graph content are created in one atomic transaction, saved and
reloaded before final readback. Invalid candidates or unsupported fields fail
before creating the target. Native creation/save failures retain the existing
rollback contract; observation warnings do not undo successful creation. Export the new asset to a
fresh workspace for subsequent edits. This limited initialization uses the
existing expression-name resolver; it does not claim complete native menus.

The final manifest projects the active identities into the default
`texture_graph` namespace. Callers import only `uai`, use `uai.connect_project()` for the local project and call methods such as
`client.texture_graph.artifact_capture(...)`,
`client.texture_graph.artifact_bake(...)`,
`client.texture_graph.graph_validate_evaluate_settle(...)`, and
`client.texture_graph.runtime_probe(...)`. Named SDK calls and the thin CLI
consume the same generated catalog, Client, Protocol and Transport. No
T40-specific Python module remains authoritative.

```text
ueagentinterfacecmd exec texture_graph.artifact.capture --params-json '{"outputs":[{"element_id":"OutputNode","output_name":"Output"}],"target_ref":"/Game/TextureGraphs/TG_Example"}'
```

Use `client.query.command_search("capture texture graph artifact")` and
then `client.query.command_usage(...)` to obtain the minimal named SDK call.
Inspect available nested parameter schemas with `ueagentinterfacecmd describe <command-id> --parameter <name>` when needed; domain semantics remain in this contract. Target
revisions and other concurrency evidence are resolved, frozen, and rechecked by
the server during the operation; callers neither read nor write them.
