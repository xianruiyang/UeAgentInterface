# T21 Audio HJSON Domain

`audio-graph-domain-owner` is the only public owner for
`audio.graph.candidates` and the T21 HJSON domain projection. Discover nodes with
`client.audio.graph_candidates(...)`. For edits, obtain
`workspace = client.content_graph.export_workspace(...)`, edit its ordinary
HJSON files and call `workspace.import_changes()`. For unknown command names or parameter shapes, follow [public command discovery](../../../../UeAgentInterfaceCMD/docs/PYTHON_SDK_AND_CLI.md#command-discovery). No task-specific helper or tutorial-owned
registry is authoritative.

`audio.graph.candidates` is read-only and target-sensitive. For a
MetaSoundSource it enumerates the native schema's frontend registry and unloaded
asset-tag declarations, returning the matching base-name list with shared query
`scope`. Querying does not load every audio asset. Same-name actions use the returned short semantic `select` fields when
needed. The service re-enumerates the
current context and fails closed on ambiguity.

For MetaSound graph editing, export the existing asset with
`content_graph.export_workspace`, edit its readable graph document, then pass
that directory to `content_graph.import_workspace`. Preserve the exported
graph context and existing nodes. A new node uses the readable candidate
`name` and optional short semantic `select`; for example, using the exact
name returned by the current frontend search:

```hjson
nodes: {
  Sine1: {
    kind: external
    name: "<name returned by search>"
    candidate: { name: "<name returned by search>" }
  }
}
```

Connections use the shared graph-local node and pin aliases from the exported
workspace. Native node identity belongs to the hidden resolver mapping, not
the editable document. The audio owner resolves the candidate through the
native schema catalog, loads and registers only the selected asset-backed class,
and delegates mutation to the existing MetaSound Builder.
The SDK and caller do not reconstruct the binding.
Asset-backed frontend classes use the native query record's asset name and
the shared path-scope disambiguation; their generated frontend class name is
not the readable selector. Ordinary graph updates retain existing native node
identity and unmodified state. Restored node removal and deleted connections
are consumed as native deletions instead of rebuilding every external node.

The current MetaSound graph document represents the default graph/page. Export
and apply bind the cached native builder to that page for the operation and
restore its previous page afterward; selecting another page in the asset editor
does not redirect a HJSON edit. Existing member `type` edits use the native
input/output type setters, including their native connection/reconstruction
semantics. Supplied input defaults use the shared recursive value codec and are
checked, with the requested member type, after native updates. Integer literals
retain exact range/integrality checks; array elements use the same scalar rules.
Ordinary `members.inputs` and `members.outputs` are complete arrays when supplied;
omitting a collection preserves it. Removing a member removes its owned native
nodes and incident connections, including those on other pages. A member of a
declared native interface cannot be removed individually, matching the native
editor's interface ownership. Member type/default and membership checks use the
original request again after compile and during final persistent readback.

Node defaults use the shared graph field `pin_defaults`, with the input alias
from the interface companion as its key. The `default_value` is the existing
MetaSound literal (`{type: "float", value: 440}` for example). Interface metadata
belongs to the companion; ordinary export no longer duplicates node input and
output arrays. Export reads the instance override when present, otherwise the
native class default on the represented page. Applying an unchanged effective
value preserves inheritance instead of creating a new override. An omitted
default preserves the native value. Older internal
`inputs[].default` payloads normalize into the same codec and native setter;
conflicting values supplied through both forms are reported.

Edit exported root properties in their `categories` entries using the ordinary
compact `value` shape. For example, a MetaSoundSource `SoundSubmixSends` array
can retain existing entries and append an entry containing only `SendLevel`;
omitted new members receive native defaults. Nested RuntimeFloat curve values
retain their complete channel shape. `UAI.Root.T85.MetaSound.CompactRoot`
verifies this public import, complete existing/default values, adjacent root
state, independent unload/reload and re-export. This evidence covers that root
container path and does not establish all Audio profiles or node properties.

MetaSound verification snapshots compare existing public graph aliases rather
than scratch-instance native node IDs. Normal export retains the native
mapping needed by the shared resolver; verification does not change that
mapping or generate another alias scheme. The native adapter also accepts its
captured edge representation when restoring a document. Node membership,
positions, connections and supplied pin defaults are also checked against the
original request after compile and during final readback. New nodes are bound
separately for the proposed and actual asset; a changed post-write snapshot does
not redefine the requested result.

The UE 5.6.1 dependency in `EnginePatches` exposes the native class catalog
without constructing an asset editor. Unloaded declarations come from the AssetRegistry
as well as the native manager cache, so discovery is independent of automatic-registration
directories. MetaSound creation selects the highest
minor within a major version, including unloaded asset declarations; the Builder
checks the actual registered class/version again when creating the node. This
class catalog does not include context-only editor actions such as variable or
interface promotion, comments, and graph clipboard operations.

SoundCue obtains its class catalog from the native Palette owner with its
ClassViewer policy. Returned classes have the existing native SoundNode creation
binding. Use the exported keyed `nodes`, readable `candidate` names
and `connections` endpoints; do not copy the native adapter's node/edge arrays
into the workspace. The graph root uses `graph.node_references.root`, whose
value is a node alias from this graph. The shared resolver owns the conversion
to native identity. Omitting the root slot preserves it, while an empty string
clears it; deleting its node requires clearing or replacing the reference in
the same import. Domain mechanics retain native ownership, connection order and
unconnected input slots. These relations, node membership/positions and the
explicit root are checked against retained request intent after native updates
and in final readback; node properties use the shared property-intent owner.
`UAI.Root.T208.SoundCue.WorkspaceGraph` verifies public candidate creation,
root replacement, second-input editing, existing native node identity and
non-target property preservation, independent disk reload and invalid-candidate
rejection. This does not claim that every SoundCue action has passed. The
ordinary document path uses incremental native mutations; the unused full-graph
reset/rebuild entry has been removed.
Node properties retain the common decoded property intent across native compile
and persistent owner replacement, resolving nodes by their existing native name.
A later native value cannot replace that original expectation. Capture failures
are reported instead of presenting an empty property list as a successful export.

The `audio_effect_preset` Reflection profile is specifically
`/Script/AudioMixer.SubmixEffectSubmixEQPreset`. Its ordinary root `Settings`
contains `EQBands`; use the exported element definition and `element_template`
to add the first band to an empty array. Existing entries, edits, and clearing
use the same recursive property transaction. This does not expose all abstract
SoundEffectPreset subclasses or route ordinary writes through the dormant
`asset.audio_effect_preset.update` mechanic.

Native editor notifications synchronize `SettingsCopy`. Independent package
replacement also rebinds captured registered submix current-chain slots with
native EQ instances, preserving their enabled state and all non-target slots.
A known incompatible current chain or a changed captured slot is rejected.
Successful replacement and checkpoint recovery reset the target instances'
transient DSP history; they restore asset settings and registration identity,
not that history. Dynamic overrides, fading old chains, and external instances
that cannot be enumerated through this current-chain contract are outside this
restoration scope. The operation does not rebuild complete chains or restart
audio devices.

The older `uai.audio_graph.edit/1` adapter description below records its internal
operation shape; it is not an ordinary SoundCue or MetaSound workspace input.
That provider is a thin adapter over the shared HJSON
import lifecycle. It owns only classification, initialization-contract
selection, schema-to-command routing, and domain completion checks. T13 is the
sole common recursive initialization authority; T21 registers only the audio
root, node, endpoint, connection, parameter, and external-reference contracts
and consumes that seam. T13 alone loads inline, single-file, and directory
carriers and owns directory recursion; T21 consumes only the normalized
`source.kind=resolved` document set, selects exactly the declared `entry_file`,
and rejects legacy top-level `carrier` and `hjson_document` fields.

The following names are internal HJSON operation kinds rather than separate
public commands. Writes delegate unchanged to `metasound.graph.interface.mutate`,
`metasound.graph.node.mutate`, `metasound.graph.link.mutate`,
`metasound.graph.default.mutate`, `metasound.graph.compile_settle`,
`sound_cue.graph.node.mutate`, `sound_cue.graph.relation.mutate`,
`sound_cue.graph.root.mutate`, and `sound_cue.graph.compile_settle`. The domain
does not introduce a second asset factory, recursive initializer, graph
compiler, persistence path, readback authority, or cleanup path.

The provider supports both missing-root creation and existing-root editing for
`/Script/Engine.SoundCue` and
`/Script/MetasoundEngine.MetaSoundSource`. Missing-root creation uses a root
`create` object and the server-owned missing expectation; creation and graph
editing are separate imports. Existing-root import uses the exported workspace,
whose hidden manifest owns the current fence, and must not carry `create`.

## Missing-root audio graph documents

Pass a complete document to `client.content_graph.import_inline(target, document)`
with a new full object target:

```hjson
schema: "uai.audio_graph.edit/1"
root: { type: "/Script/MetasoundEngine.MetaSoundSource", create: {} }
```

For SoundCue, use `root.type: "/Script/Engine.SoundCue"` in the same shape.
The server derives asset name/package and invokes the native factory/defaults;
no `root.operation` is needed for creation. Export before graph editing.
This domain's root types do not include SoundSubmix or MetaSound Patch.

The MetaSound semantic-workspace path validates through the domain owner and
uses the shared transaction to stage, save, unload, reload and validate the asset
before committing. Domain callbacks provide native rebinding and readback;
there is no second post-commit save/reload on this path. Public re-export reads
the graph through the normal export path.
`UAI.Root.T85.MetaSound.FrontendVertical` performs that explicit unload,
absence assertion, disk load, and public re-export check for a Sine node
connected to an Audio graph output. This verifies that representative success
path, not full native-menu coverage or every MetaSound edit.
Before mutation, the same durable coordinator captures both the original disk
artifacts and a package checkpoint of the current memory state. A late readback
failure restores memory from that checkpoint, restores the original disk and
dirty state, and compares the recovered semantic state before confirming
rollback. Unconfirmed restoration preserves the journal and both before-images.
The test also rejects final readback through the production apply seam and checks
a dirty Volume value and the original disk bytes independently. This is bounded
recovery evidence; successful worker cleanup alone does not establish rollback.
