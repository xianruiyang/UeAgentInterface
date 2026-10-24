# T19 PCG HJSON domain

`pcg-domain-provider` is the sole `content_graph.*` domain provider for
`/Script/PCG.PCGGraph`. Discover nodes with
`client.pcg.node_candidates(...)`. For edits, obtain
`workspace = client.content_graph.export_workspace(...)`, edit its ordinary
HJSON files and call `workspace.import_changes()`; the schema is
`uai.pcg.edit/1`. For unknown command names or parameter shapes, follow [public command discovery](../../../../UeAgentInterfaceCMD/docs/PYTHON_SDK_AND_CLI.md#command-discovery).

The shared source loader owns inline, single-file, and directory carriers. T19
consumes only the normalized `source.kind=resolved` document set and does not
accept legacy top-level `carrier` or `hjson_document` fields.

T13 remains the sole common recursive initialization authority. T19 registers
only PCG root, node, endpoint, and parameter initialization contracts and
consumes that shared seam; it does not create another recursive dispatcher.

## Root lifecycle

The complete minimal initialization document is:

```hjson
schema: "uai.pcg.edit/1"
root: { type: "/Script/PCG.PCGGraph", create: {} }
```

Pass it to `client.content_graph.import_inline(target, document)` with a new
full object target. The server derives name and package from that target;
`root.create` needs no duplicated path fields. Native creation supplies defaults.

A new PCG Graph uses a root `create` object; the server owns the missing-target expectation.
The public target owns the final package path and asset name; conflicting
declarations fail before write. Root creation delegates to the existing
`pcg.graph.asset.create` owner and cannot be combined with an edit in the same
import. Create the root first, export its ordinary workspace, then edit and
import that workspace; its hidden manifest owns the current revision.

For an existing graph, export its `llm_authoring` workspace, edit the visible
graph `nodes`, `connections`, or declared properties, and import that directory.
The hidden manifest owns identity and concurrency state. Keep existing entries
when adding a node; the example below shows only the new `nodes` member.
The semantic projection uses the existing PCG graph mechanics; callers do not
construct native operation envelopes or edit hidden metadata.
Unchanged nodes with the same identity and settings type retain their native
settings. Edit a node's exported `categories` in its `properties.hjson` to change
declared properties; those edits use the common typed property projection.
Adding another node or changing one property preserves the other settings and
the resolved connections.

Root and node properties consume the common compact value completion. Edit
the exported value shape directly, including appending entries to arrays such
as `PointsToCreate`; internal typed wrappers are restored by the server.
For ordinary reflected structs, editable members remain available when the
struct also contains read-only members. Read-only members are omitted from
the editable value: existing values are preserved and new elements receive
native defaults. Explicit writes to read-only members are rejected. Omitted
editable members of a newly appended element also use native defaults.
Root properties use the root object's exported `properties.hjson` and the same
category/type/value contract. Root changes such as `bLandscapeUsesMetadata`
can be imported together with node properties; both are verified after an
independent package reload and re-export.

Graph compilation is owned by the native update phase. UE 5.6 `Recompile()`
reports whether the compiled result changed, not success; UAI refreshes the native
cache, checks compiler availability and preserves synchronous `LogPCG` errors and
warnings. It does not depend on private engine compiler headers or claim that a
cached graph proves successful generation. The graph apply backend no longer
performs an additional unchecked recompile.

## Candidate-to-authoring chain

`pcg.node.candidates` constructs the native PCG Editor graph context for the
target `UPCGGraph` and enumerates its background right-click menu through
`UEdGraphSchema::GetGraphContextActions`. The public result is the complete matching view of
that complete native action owner, not a global `UPCGSettings` class catalog.
The default model-facing result is `{"names":[...]}`, with deduplicated readable
names of matching native actions. Internal action identity, presentation metadata, action kind,
revisions and native paths stay on the machine side. Search
coverage and creation coverage are separate: the complete background menu is
searchable, while only `FPCGEditorGraphSchemaAction_NewNativeElement` is
currently enabled for creation.

For a selected name, `describe={"name": "<returned name>", "fields": [...]}`
reports `description.creation.supported/reason` and available property defaults.
Unsupported actions remain describable without mutation; names-only discovery
does not imply that every native menu action can be created through HJSON.

Native menu aliases with the same creation inputs share an identity. Search keeps
each alias's name and metadata, then emits one representative per matching
creation input; either original name remains valid for description and creation.
For supported native actions, equivalence includes the settings class and both
the preconfiguration index and label. Different creation inputs are not merged;
an identity collision between them remains an error.

Query results also deduplicate original names; names do
not acquire scope suffixes. The minimal HJSON selector is
`candidate: {name: ...}`. If the service returns `candidate_ambiguous`, keep
that original `name` and copy a short selection from `choose` into `select`.
PCG uses readable native settings semantics when they distinguish the actions.
The shared resolver rechecks the selection against the current native context;
stale choices return `candidate_selector_invalid`. The SDK's public
`Result.error` retains the original `code`, `name` and available `choose`
through ordinary directory import. Do not copy the complete search row, a settings class path, an
internal revision, the semantic node identity, or placement into `candidate`.

```hjson
nodes: {
  MyGrid: {
    candidate: { name: "<exact name returned by node_candidates>" }
    position: [160, 96]
  }
}
```

Preflight re-enumerates the same native graph context and resolves one exact
action. Commit re-enumerates again and invokes that action's public
`PerformAction`; it does not map the selector to a second settings-class
constructor. The returned native node supplies the immediate committed state,
which is then checked through save, unload/reload, and independent readback.
Missing, ambiguous, wrong-context, unsupported-action, or non-conjunctive
selectors fail before the first planned write.

## Lifecycle and readback

Use the node alias from a fresh ordinary export with
`client.content_graph.items(target, input={"items": [alias]})`.
Node reads share the export snapshot and common graph projection, including
`name`, native `node_type`, authored title and position. Request
`fields: ["name", "node_type", "pin_interface"]` when the current Pin interface
is needed; the default item response omits that larger field. Its `pins[].alias`
uses the same shared projection as exported HJSON connection endpoints.

Both root creation and mutation must complete the real mutation, settled compile
state, saved package, unload/reload, and independent persisted `ContentGraphRead`
snapshot. Their public success result is a short business projection containing
the terminal status; revision, persistence receipt, and
readback evidence remain server-owned lifecycle facts. Unsupported schema/root/operation combinations,
stale revisions, target mismatches, invalid candidates, and lifecycle failures
fail explicitly without a legacy fallback. `content_graph.export_workspace`
publishes the registered PCG Graph authoring workspace. Semantic-directory
import includes declared root UObject properties and the native graph
projection; it is not limited to root properties.

For existing-asset semantic updates, final persisted readback runs inside the
shared transaction before commit. The common package checkpoint restores the
pre-import in-memory graph, including dirty node settings and connections;
the independent disk backup restores the original saved file. This update
lifecycle does not establish equivalent failure coverage for asset creation,
cloning, or deletion.
