# T18 Niagara HJSON domain

`niagara-domain-provider` is the sole `content_graph.*` domain provider for
`/Script/Niagara.NiagaraSystem`, `/Script/Niagara.NiagaraEmitter`, and
`/Script/Niagara.NiagaraScript`. The import schema is
`uai.niagara.edit/1`.

The public SDK uses `client.niagara.module_candidates(...)`,
`client.content_graph.import_inline(...)` for new documents, and
`workspace.import_changes()` for an exported workspace. Use
`client.query.command_search(...)`, the generated SDK and the formal schema to
inspect their live contracts. The CLI equivalent is `ueagentinterfacecmd exec`, which
forwards the same exact generated binding without a task-local helper. No
legacy Niagara command or tutorial registry is an ingress or fallback.

`client.content_graph.items(target)` includes a read-only `input_schema` on
captured Niagara modules. The query and workspace definition catalog consume
the same native input definition builder. Its keys are accepted input names;
each row contains `type` and `contract`. The contract gives literal support,
`kind`, the shared enum `candidates_header`/`candidates` rows, and the native `default`
payload when representable. A default payload can be copied to the corresponding
workspace `inputs` entry and edited; it already uses the accepted `{kind, value}`
or linked-input shape. `default_source` distinguishes assignment, script literal,
binding and computed graph defaults. Computed defaults are not replaced with zero.
The workspace exposes these definitions through its normal field-definition
references, including `definition` on folded `modules` rows. These references
are navigation metadata and are not applied as module fields. Query numeric
defaults remain JSON numbers, using the same payload types as the workspace.
Editable `inputs` remains the override map, so merely reading a definition or
exporting a default does not author an override. A value equal to the native
default may read back without an explicit override; consult the definition for
its effective default. Final verification compares an explicit default with the
effective value established by the current native capture, while keeping the
exported override map sparse. A schema default alone does not prove that a missing
value was retained; computed or unavailable defaults are not invented.
Omitting an input from an edit does not request a reset.
Captured DynamicInput workspace units and their nested dynamic-input children
use the same input definition builder for each script's inputs. Their
`input_name` identifies the host binding; use each child's definition for its
own input names and defaults.

The same `contract` also carries optional native `description` and `display_unit`.
These remain inside the contract so the final workspace definition retains them
as `value_definition` metadata, including when the input uses a linked default.
Function modules and dynamic inputs read metadata from their currently selected
script version's graph. Assignment inputs use Niagara constant metadata or the
owning System/Emitter's subscribed parameter definitions, matching the native
stack's lookup order. Subscription lookup uses the exported native getter and
matches the variable's name and type. Definitions with different descriptions or units remain
distinct even when their numeric type is the same.

`display_unit` is the native unit display symbol, not an instruction to convert
values: Niagara declares it as a visual indicator only. Missing metadata does not
mean dimensionless. Read native descriptions and mode/enum selections for conditional
angle interpretation; degrees, radians and normalized turns are not interchangeable.
Changing a selected script version or interpretation mode requires reading its
current definition again. Generic vector inputs, including YPR-named inputs, do
not inherit Rotator semantics merely from their name. These metadata fields are
read-only and are never copied into editable `inputs` overrides.

## Import contract

The shared source loader owns inline, single-file, and directory carriers. The
domain consumes only its normalized `source.kind=resolved` document set. The
HJSON entry contains one exact root `type` and a `document` containing the
operation, target and optional properties or provider extension. For example,
the verified Emitter text creation document is:

```hjson
{
  "schema": "uai.niagara.edit/1",
  "root": {
    "type": "/Script/Niagara.NiagaraEmitter",
    "document": {
      "operation": "create",
      "target": { "asset_path": "/Game/Example/NE_Text", "asset_kind": "emitter" },
      "properties": [
        { "property_path": "TemplateAssetDescription", "value": { "literal": "Created emitter source" } }
      ]
    }
  }
}
```

Pass this document through the public inline source with the matching request
target. The server derives the missing-target fence; callers do not provide
`expected_revision` or other machine context.

T13 remains the sole common recursive initialization authority. T18 registers
only the Niagara root and owned-entry initialization contracts and consumes
that shared seam; it does not create another recursive dispatcher.

- A new root uses `document.operation: create` or `clone`; the server owns the
  missing-target expectation.
- Edit an existing root through its exported workspace and `import_changes()`;
  that workspace owns the current baseline. `import_inline` initializes a
  missing target; do not reuse it for an existing-root update or supply a revision.
- `document.target.asset_path` must identify the public request target;
  `asset_kind` is normalized to `system`, `emitter`, or `script` from the exact
  root type.
- Compile and save are mandatory. Explicit `compile: false` or `save: false`
  fails before mutation.

The domain calls the single `niagara.apply` authoring provider for root
lifecycle, stages, modules, inputs, user parameters, renderer/event structures,
and recursive dynamic inputs. It does not copy the provider's parser,
transaction, rollback, compilation, persistence, or readback mechanics.

## Candidate-to-authoring chain

`niagara.module.candidates` currently accepts one persisted Niagara System,
readable `target_usage`, and the readable stage discriminators needed by that
System: optional `emitter_name` and `stage_name`. The
owner builds a data-processing-only `FNiagaraSystemViewModel`, finds the exact
`UNiagaraStackScriptItemGroup`, and calls that group's native
`GenerateAddActions`. This is the same Add Module action set used by the Stack
UI; it does not open an asset editor, preview, or window.

The result is the complete matching name list plus one response-level `scope`. Asset modules are currently
executable. Scratch-pad modules, New Scratch Pad, assignment/set-specific, and
wizard actions remain non-creatable; dynamic inputs are a different native
creation surface and are not part of this command.

Optional `describe={"name": "<returned name>", "select": {...}}` in the same stack context
returns `description.creation.supported/reason`, including unsupported actions.
Default search returns all matching base names and one shared `scope`, including
actions without creation support. Actual source-enumeration gaps still return
`candidate_catalog_incomplete`; creation support remains a separate selected-action result.
Omit `select` unless needed to resolve an
actual same-name ambiguity. Description creates no module; its input defaults
still require the actual created stack context.

For a script module in an exported ordinary workspace, add the readable
candidate name directly to the new row, alongside placement and inputs:

```hjson
Collision: {
  name: Collision
  module_index: 5
}
```

The shared keyed-item projection builds the internal candidate envelope.
Do not paste `candidate: { name: ... }` into this ordinary row. Use the
workspace's exported input descriptors for values; Niagara literal vectors
use an array in `value`, whereas a canonical UObject vector descriptor uses
`components`. These are distinct value contracts.

Assignment modules use their native constructor arguments directly, without a
candidate name. Merge a new row such as
`AssignLifetime: { module_kind: "assignment", assignment_targets: { "Particles.Lifetime": null } }`.
Known native attributes allow `null` to infer their type; custom variables need
an explicit type. The refreshed workspace reports resolved types and inputs.
This path does not execute the interactive “Set new or existing parameter” action.

To remove an existing module, delete its entry from the explicitly supplied
module collection. The shared owner generates the internal remove operation.
`remove: true` belongs to the inline provider-extension shape below.

An exported System workspace also exposes complete keyed `emitters` and
`user_parameters` collections. Follow its current refs. Add an emitter row with
a readable instance key and `source_asset_path`, optionally `enabled`; import
refresh creates its actual child refs. An enabled-only edit preserves local
content. Explicit `source_sync_mode: merge`/`replace` synchronizes the instance's
original source/version. Removing the entry removes the System handle, leaving
the source asset intact. Add a parameter row such as
`User.Intensity: { type: "float", value: { literal: 2.5 } }`; remove its entry to
delete it. Derived `kind`/`value_text` do not belong to ordinary edits.
Existing EventHandler and DynamicInput units use their exported field shapes
and retain their name/binding identity. These edits share the native provider;
they do not imply arbitrary event creation or Script topology support. See
[the ordinary contract](../hjson-authoring/08_NiagaraApplyDomain.md#7-export-与-apply-公共入口).

For a new inline domain document, the provider extension uses the explicit
candidate envelope instead:

```hjson
provider_extension: {
  stages: {
    "SystemUpdateScript": {
      target: {
        usage: SystemUpdateScript
      }
      modules: {
        native_asset_module: {
          candidate: { name: "Jitter Position" }
        }
      }
    }
  }
}
```

`candidate` contains the returned readable base `name` and, when needed, the
short semantic `select` fields returned for same-name native actions. Stage, emitter, ordering, operation,
inputs, and placement stay
outside `candidate`. The server resolves the exact stage and re-enumerates the
same native Add Module action set during preflight and again immediately before
the first write. A supported asset-module candidate is then executed through
the same native `ExecuteAddAction`; callers never copy a script path, binding
revision, or complete search row. Missing, ambiguous, context-mismatched, or
unsupported actions fail before mutation.

## Lifecycle and readback

Success requires the mature authoring provider to finish mutation, compilation,
semantic verification, save, and independent readback. The public success
result is a short business projection containing the terminal status and
`target_ref`; revision, persistence receipt, and readback evidence remain
server-owned lifecycle facts.

`content_graph.export_workspace` publishes the registered Niagara authoring
workspace for System, Emitter and Script targets. Semantic-directory import
projects captured emitter struct property changes through the common property
completion and apply contracts into the existing Niagara emitter lifecycle.
Edit the ordinary exported property value; the workspace retains the emitter
binding. Omitted struct members retain their current values through compact
completion. Stack, module, dynamic-input and graph structures retain their
Niagara-native lifecycle. The replaced
`niagara.stack.apply` and `niagara.script.graph.apply` commands are inactive;
there is no legacy provider or parallel Niagara HJSON runtime.

The standalone Emitter `FixedBounds` case is verified through public compact
import, independent unload/reload, complete bounds and non-target module/pin
preservation, and public re-export. A separate injection at the same compact
apply provider verifies rejection after persisted reload and restoration of
both dirty owned content and the different original disk image. Strict typed
inputs and internal recovery retain their existing value contract. This case
does not establish renderer or data-interface coverage.

A System containing two unversioned embedded emitters is also verified: an edit
selected through one readable owner directory preserves both handles, data-version
association, non-target native content and pins after independent reload. The
public re-export must match the predetermined single-leaf change exactly. An
unversioned empty version selector is valid under the native API; preflight uses
the already resolved emitter data. Existing apply may normalize that selector to
the exposed version while preserving data identity. This does not establish
multi-version emitter coverage.

Model/export capture also includes native editable persistent root properties
through `core.property`; full before-image and verification capture retain their
separate lifecycle purpose. A System root `bRequireCurrentFrameData` edit from
false to true is verified through public directory import, independent reload,
preservation of other editable root fields and both embedded emitters, and exact
public re-export. Edit the exported bool's `value` while retaining its declared
`type`. Root ownership uses the object path; package identity remains part of
persistence.

A standalone Emitter root `TemplateAssetDescription` edit is separately verified
through the common FText path. Edit its ordinary source string and retain its
declared `text` type. Two successive edits persist through independent reload
and exact public re-export while native module content is preserved. An unchanged
directory returns `hjson_import_no_explicit_changes` without changing text
identity, asset revision, or disk bytes. This does not establish Script root
properties or all nested text types.

Emitter text creation into an absent target is separately verified through
public inline import, native factory content, independent unload/reload and
ordinary source re-export. Public inline import now uses the shared compact
value completion and atomic final readback. Existence lookup retains loaded
objects and checks native package availability before loading, so a missing
target does not acquire an empty package during preflight. Rejection during
checkpoint capture, before forward save, and after clean persistent reload
restores the created object, package, registry
entry and disk to absence; the asset remains absent on the following editor
tick. This does not establish other root types.

Emitter Clone is verified with both a newly factory-created source and an
independently loaded persistent source. The source may contain unsaved changes;
the clone uses that current content while preserving the source object, dirty
state, registration and original disk bytes. Specify the source through
`document.target.source_asset_path` and the new destination through
`document.target.asset_path`. Explicit text edits, native owned content and
references survive independent target reload and ordinary re-export.

Publication uses the native Emitter name synchronization API so the destination
name and parameter aliases agree. Native duplication may initialize previously
invalid pin identities; existing valid identities and connections remain
protected. A late refusal after a fresh persisted target restores target absence
and preserves the dirty source, including on the following editor tick. This
recovery case uses an independently loaded source; it does not establish every
source-state combination, multi-version emitters or Script root properties.

A standalone Script's `ValidationRules` uses the common owned-object array
representation. Editing an existing No Events rule's `Severity` in the ordinary
exported directory is verified through public import and exact re-export, while
the rule's `Platforms`, a second rule, native Script content and graph pins remain
unchanged. Separate public deletion cases remove the first rule and retain the
second rule's original owned identity, including when both rules have the same
class. External serialized references follow that retained rule through public
import and an independent package reload; exact re-export preserves the intended
remaining content. Existing elements use the common readable `owned_object.instance`
binding described in the plugin's `docs/UObjectPropertyValueV2.md`.
An additional case creates a distinct same-class rule using the queried `name`
and explicit non-default properties, while preserving the original rules and references.
Explicit `[]` clearing is also verified, including removal of the old objects and
clearing their external references. Both cases verify independent reload and exact
re-export.

Minimal native creation is also verified for this Script property:

```python
result = client.asset.owned_candidates(
    target, {"property_path": "ValidationRules", "query": "Emitter Count"}
)
if not result.ok:
    raise RuntimeError(result.error)
new_item = {"owned_object": {"name": result.payload["names"][0]}}
```

Append that item to the exported ordinary ValidationRules array, then call
`workspace.import_changes()` and export to a separate readback workspace.
Use the returned name unchanged, including its current localization, for example
`{owned_object: {name: "Emitter Count"}}`; do not copy an existing instance key.
The native PropertyEditor picker and creation helper supply the concrete type and
defaults. The normal case preserves both existing rules, their external references
and all other native Script content through independent package replacement and
exact ordinary re-export. Existing items expose `instance` and `properties`;
their exact class is recovered from the captured instance. Ordinary dynamic owned
arrays reject `class_path` inputs, with or without `instance`, before writing.
Existing items cannot reselect their class with `name/select`.
Unknown-name refusal is verified through public import.
A separate prepared-apply case makes only the real Script read-only after prepare
and observes rejection before any real Modify or slot write, before rollback.
The native-create transaction also verifies rejection after clean fresh persisted
readback, restoring full dirty memory, references and the different original disk.
These results do not establish other owner/property contexts, changes across a
batch of properties, or unloaded Blueprint rule types.

Script saves select the existing native resident-package replacement strategy:
compiler-owned references can keep an old Script alive during ordinary unload.
Readback must therefore establish a new persisted package and object, remap
references and verify content; an unload return value alone is insufficient.
The Script case also verifies a late rejection after clean new-object readback,
restoring the full dirty owned state and the different original disk bytes.
The structural deletion recovery case additionally restores the removed rule,
original order, graph and pins, and external reference binding before cleanup.
