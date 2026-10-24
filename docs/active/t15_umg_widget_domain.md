# T15 UMG/Widget Domain

To create a generic function graph in an existing asset, edit the always-exported
`graph_declarations` map with a readable name and `{ graph_kind: "function_graph" }`.
Import and read the refreshed workspace before querying `client.blueprint.node_candidates(...)`
for that new graph and adding its returned node name in a second ordinary import.
Export again only if the workspace was not refreshed. Graph-body
name/kind are read-only references; nodes, pins, edges and layout stay in the
body. Omit a function or macro declaration to remove that graph; leave its old,
unedited body files in the workspace. Removing a declaration while editing its
body is rejected before writing. Event and animation-specific graph protections
remain native. Changing a map key means remove plus create, not identity-preserving
rename; kind conversion is unsupported.
Native identities remain server-owned; no extra files or manifest edits are needed.


`workspace.import_changes()` invokes the shared `content_graph.import_workspace`
Widget Blueprint HJSON mutation ingress. Its `source` union accepts `inline`,
`single_file`, and `directory` carriers; no UMG-specific apply command is
public. `client.umg.widget_candidates(...)` is the public read-only creation
query for one target Widget Blueprint. Shared `content_graph.*` reads and
export are the public persisted inspection paths. For unknown command names or parameter shapes, follow [public command discovery](../../../../UeAgentInterfaceCMD/docs/PYTHON_SDK_AND_CLI.md#command-discovery). Existing non-HJSON UMG asset creation and widget-event
discovery commands keep their separate owners.

The accepted entry document is `uai.umg.edit/1` with exact root type
`/Script/UMGEditor.WidgetBlueprint`. The provider owns Widget Blueprint
settings and members, widget tree, native slot layout, bindings, animations,
recursive properties, and managed Blueprint graphs. Its content-graph readback
identifies widgets, slots, bindings, animations, hierarchy relations,
recursive properties, compile diagnostics, and persisted revision.

## Contextual widget creation

`umg.widget.candidates` resolves its internal identities against the exact
target Widget Blueprint and target revision. Its default result is
`{"names":[...]}`, with every matching readable name; class paths,
coverage fields, revisions and hidden identities remain server-side.
Optional `describe={"name": "<returned name>"}` reports
`description.creation.supported/reason` even for unsupported actions, without
creating a widget. The class/template catalog includes native widgets and Widget
Blueprint assets, including unloaded generated classes. It applies the native
Palette policy in the target Widget Blueprint context. Description loads only the
selected class and exposes its writable property defaults; creation uses that same
class through the native widget template. Querying does not open an asset editor.

The minimum HJSON selector is `candidate: {name: "Button"}`. Query names are
deduplicated and retain their original spelling without scope suffixes.
The shared resolver returns `candidate_ambiguous` with `name` and a minimal
`choose` when a semantic distinction is available. Retry that name with `select`;
an invalid selection returns `candidate_selector_invalid` with current recovery data.
UMG declares `form` as `native widget` or `widget blueprint` from the actual creation
kind; collisions without sufficient semantic distinctions remain explicit
`candidate_discriminator_missing` failures. Preflight and apply preserve the
mutation's code and detail.
A caller that already knows the class may retain `class` only as an
assertion; class is not required to create the widget. Instance identity,
parent and slot order remain outside the candidate:

The normal SDK workflow exports a workspace and edits the ordinary document
containing `widgets`. It is a complete desired map: retain existing keys to keep
widgets, remove a key to delete it, and add a key with the queried candidate name
to create a widget. New entries use the map key as the instance name:

```hjson
widgets: {
  RootCanvas: { make_variable: false }
  ActionButton: { name: "Button", parent: "RootCanvas", index: 0 }
}
```

Only include `RootCanvas` when it was captured or create it with its queried name.
Existing entries omit the candidate name; their native identity and class are restored
from the capture. Each widget's `properties.hjson` contains its own properties;
`slot/properties.hjson` contains its parent's layout-slot properties. These distinct
native objects may have identically named fields; edit them in their own documents.
`index` is an integer sibling position. Native bounds clamping is preserved;
conflicting final positions are diagnosed instead of silently accepting the last move.
Blueprint asset settings and generated-class defaults are separate captured property
surfaces. Edit their ordinary exported fields; root properties use the shared
Blueprint root-property enumeration, while class defaults use the existing
Blueprint class-default mutation owner. Widget and graph identity remain captured.
The UMG adapter consumes shared keyed-item planning and sends resolved changes to
the original WidgetTree mutation owner. Final readback runs inside the common
transaction before commit. Saved updates release their native references before
the shared package reload and rebind the Widget Blueprint afterward. A final
readback failure restores the transaction's original package checkpoint and disk
artifacts. Existing Blueprint graph nodes use the shared Blueprint semantic
projection: edit the exported pin property values and the node's ordinary `pos`,
then import the workspace and export a separate readback. The UMG adapter keeps
widget and slot changes with their existing owner and passes graph and class-default
changes to the Blueprint owner. The ordinary `variables` map in `member_collection`
uses the same Blueprint keyed-member projection and prepared mutation. Follow
the [shared Blueprint member type contract](t14_blueprint_surfaces.md#shared-blueprint-member-variable-creation).
Retain existing entries and add a readable variable name with its `type`, for example
`SdkCount: {type: {pin_category: "int"}, instance_editable: true,
default_property: {property_path: "SdkCount", value: 17}}`. The server retains the
native-created member identity for actual application. After creation, independently
export and edit the returned member fields or generated-class default property.
Other unverified member forms remain outside this variable slice.

The ordinary `bindings` map in `binding_collection` contains binding records,
not widget candidates. For an existing TextBlock and compatible FText member,
add `DisplayText_Text: {widget_name: "DisplayText", property_name: "Text",
binding_kind: "property_variable", source_variable_name: "SourceA"}`. Independently
export after creation, then change `source_variable_name` to another compatible
member. Existing widget/property identity stays bound to the captured record;
remove its key to delete the binding. Retain other entries. The common keyed-item
resolver restores identity and plans operations; UMG applies bindings after its
first compile and compiles again before saved readback. Function bindings use the
existing native function branch and require a compatible function; this property-
variable slice does not establish the function-binding matrix.

Existing animations expose an ordinary `animations` map in `animation_collection`.
The map and its animation/binding/track values may use `content_ref` to ordinary
workspace-relative content files. Follow those references and edit their `value`;
the same common timeline organizer is used by LevelSequence. Sections, channels
and keys of a track remain together. These files assemble into the existing map
before keyed desired-state resolution; creation, removal and native identity rules
below are unchanged, and new map entries can be authored inline.
Retain its animation and binding entries. Within the captured track, section and
channel maps, edit key `value_lexeme`, exported key `properties`, or the channel
`default` (`is_set` and its value). Native binding, track, section and channel
identities are restored by the shared keyed-item projection. UMG supplies its
widget/slot binding context to the common MovieScene capture and prepared value
update owner, also consumed by LevelSequence. Existing edits preserve the native
MovieScene rather than replacing it from a first-section summary. The opacity
slice covers multiple sections, preserved non-target data, persisted readback,
and late rejection restoring dirty memory and original disk separately.
Add a named entry such as `Added: {display_label: "Added animation"}` to create
an empty animation using native MovieScene defaults. A new entry may also contain
the common `bindings` and `root_tracks` maps and scene settings in the same import.
For example, `Added: {bindings: {Root: {binding_kind: "root_widget", object_ref: "",
tracks: {}}}, root_tracks: {}}` creates the animation and its root widget binding.
Omitted scene settings come from the newly created native MovieScene; callers do
not supply private clock identity. Read the refreshed workspace for native defaults before
editing track/section/channel/key fields. The animation type is fixed by the owning
collection; no class or GUID is supplied. Remove an animation entry to delete it,
retaining other entries. New property tracks use `timeline.track_candidates` with
`animation_name` and, for bound properties, `binding_kind`, `object_ref` and
`property_path`. Use the returned candidate `name`; shared MovieScene creation,
property context and section defaults are the same owner as LevelSequence.
Deleting a Designer widget uses native UE deletion and preserves animation
bindings/tracks whose widget is now missing. Remove those entries from the
animation's `bindings` map to explicitly delete the corresponding tracks.
Existing animation entries also expose `display_rate` (`numerator`/`denominator`)
and exact `playback_range` bounds. Change the exported `point.tick` using its
displayed rate, retaining the bound kind. Hidden context supplies clock identity.
Display-rate edits preserve key and section timing; playback edits change only
the native bounds. An inclusive playback upper bound persists as the equivalent
exclusive next tick; overflow, open, inverted or mismatched-clock playback ranges
fail before publication. New empty animations use the persistent empty range
`[0,0)` with native rate defaults. Tick-resolution retiming remains outside this slice.
Missing-target inline animation initialization retains its existing syntax; its
native channel writes use the same prepared scalar owner and its prepared native
identities are retained for actual application.

To add a node to an existing graph, query `client.blueprint.node_candidates(...)` with the
Widget Blueprint target and readable graph name. Copy the returned name into
the new ordinary node entry, for example `SdkPrint: {name: "Print String",
pos: {x: 512, y: 96}}`. The same Blueprint candidate normalizer and native action
owner resolve it before mutation. Native placement may snap to the graph grid;
read the refreshed workspace for the settled node interface and editable pin properties.
The prepared Blueprint mutation retains identities allocated by the native
creation, including companion nodes and their pins, for its actual application.
Existing node and pin identities remain unchanged; callers do not provide GUIDs.

The lower-level `uai.umg.edit/1` document uses the resolved provider representation:

```hjson
provider_extension: {
  widget_tree: {
    widgets: {
      "widget:ActionButton": {
        operation: create
        candidate: { name: "Button" }
        parent: RootCanvas
        index: 0
      }
    }
  }
}
```

Before the first WidgetTree mutation, the server re-enumerates the same target
context and resolves the selector again. Creation then uses the public
`FWidgetTemplateClass` creation mechanics, including unique naming and
`CreatedFromPalette()`. The existing UMG authoring owner remains responsible
for parent/slot insertion, compile, save, unload/reload and independent
readback; search results do not carry those placement responsibilities.

The UE 5.6.1 dependency in `EnginePatches` exposes the existing native Palette
filter with a Widget Blueprint context. Class and Blueprint template discovery
share that policy and the public asset registry. Non-class Designer templates and
UI-only actions remain outside this class-candidate contract; the remaining
Designer-wide equivalence boundary is recorded in
`DCR-UAI-UMG-PALETTE-CATALOG-OWNER`.

T13 is the sole carrier-loading and common recursive initialization authority.
It validates and recursively loads the public source, resolves one entry file,
binds the exact target identity, and passes a normalized `kind: "resolved"`
source containing the workspace documents to T15. T15 rejects legacy top-level
carrier or document fields, selects exactly one document matching `entry_file`,
parses only that entry, and registers only the Widget Blueprint root plus its
owned widget, slot, binding, and animation initialization contracts. It does
not implement a second parser, directory walker, recursion engine, reference
binder, rollback path, or carrier cleanup path.

Create/update/upsert use the target and exported workspace; the hidden manifest
and server lifecycle own missing/existing expectations and the latest fence.
The `uai.umg.edit/1` inline path also performs its final readback inside the
existing authoring transaction, after native Widget Blueprint compile, save,
package unload and reload, and before commit. Saved creates use an absent
package checkpoint; final readback rejection removes the published asset.
Saved updates restore the original dirty memory and disk artifacts separately.
The SDK entry is `client.content_graph.import_inline(target, document)`;
use a separate workspace export to inspect the persisted result.
Inline and single-file sources initialize missing targets. Existing targets
require an exported directory workspace with its hidden identity mapping and
base revision; inline update/upsert is rejected by the revision fence before
apply. Lifecycle Update support does not bypass this carrier contract.

The complete missing-root Widget Blueprint document is:

```hjson
schema: "uai.umg.edit/1"
root: {
  type: "/Script/UMGEditor.WidgetBlueprint"
  document: {
    operation: "create"
    target: { asset_path: "/Game/Example/WBP_New", asset_kind: "widget_blueprint" }
    provider_extension: { parent_class: "/Script/UMG.UserWidget" }
  }
}
```

Pass it to `client.content_graph.import_inline("/Game/Example/WBP_New.WBP_New", document)`.
The target package and object reference must match. `parent_class` is optional
and defaults to native UserWidget; then export for ordinary content editing.
Unsupported roots or creation roles, source identity or entry
mismatch, stale revisions, unresolved widget/slot classes, invalid hierarchy
or bindings, and compile or lifecycle failures are typed terminal failures
without a retired-owner fallback or residue.

## Widget event discovery

`umg.widget.events.discover` is a names-only query. Its public result is
`{names:[...]}` with every matching readable event name after query filtering;
event references, signature paths, target references, hashes, GUIDs, and other
machine identities are not returned. Names are resolved in the same widget
owner context. A matched ambiguous name fails closed, while an unrelated
ambiguous group does not block a focused query.
