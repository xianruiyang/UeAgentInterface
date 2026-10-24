# T22 Sequence HJSON domain

Discover the current command with positive keywords through
`client.query.command_search(...)`, then read its exact usage with
`client.query.command_usage(...)`. Do not use a separate authoring-schema
command or a static template as the authoring contract.

The normal flow is to export the target with
`workspace = client.content_graph.export_workspace(...)`. The exported HJSON is the source
of truth for editable fields and their type/value definitions. Edit the ordinary
HJSON files, then submit the directory through
`workspace.import_changes()`. Hidden workspace files retain
machine identity and concurrency information; callers do not edit them or
supply `expected_revision`.

`content_graph.import_workspace` remains the only public HJSON write contract.
Exported directories support existing-asset edits. Inline and single-file
sources are reserved for missing targets. Missing-target initialization now
requires `structure`; the retired `fields` model is rejected before creation.
Initialization uses the common transaction through save, reload and final readback. The
verified inline case includes possessable bindings, root and bound float
tracks, sections and keys. If staging or final readback fails, creation
restores the original absence of the asset in memory, the registry and on disk.
The Sequence provider owns its domain mechanics,
while common source resolution, validation, transaction, persistence and
readback remain shared infrastructure.

## Missing-root Level Sequence

Submit the complete document through
`client.content_graph.import_inline(target, document)`. For destination
`/Game/Example/LS_New.LS_New`, the following authors an empty one-second
sequence at 30 display frames per second; these are chosen example values,
not a claim about UE factory defaults:

```hjson
schema: "uai.sequence.edit/1"
root: {
  type: "/Script/LevelSequence.LevelSequence"
  structure: {
    display_rate: { numerator: "30", denominator: "1" }
    tick_resolution: { numerator: "24000", denominator: "1" }
    playback_range: {
      clock_id: "level_sequence:/Game/Example/LS_New.LS_New"
      state: "authored"
      lower: {
        kind: "inclusive"
        point: {
          clock_id: "level_sequence:/Game/Example/LS_New.LS_New"
          tick: "0"
          rate: { numerator: "24000", denominator: "1" }
          state: "authored"
        }
      }
      upper: {
        kind: "exclusive"
        point: {
          clock_id: "level_sequence:/Game/Example/LS_New.LS_New"
          tick: "24000"
          rate: { numerator: "24000", denominator: "1" }
          state: "authored"
        }
      }
    }
    bindings: []
    root_tracks: []
  }
}
```

Every `clock_id` is `level_sequence:` followed by the destination's full
object path. Point rates must equal `tick_resolution`; the playback range
requires both bounds. Replace the example destination consistently.
Creation uses `structure`, not `create`, `fields` or a root `operation`.
Export the created asset to obtain its ordinary editable workspace.

## Existing assets

Existing Level Sequence root properties use the common category and type/value
format. For example, `AssetUserData` is an editable array and accepts a null
slot. Root-property import saves and reloads the asset before returning;
unedited MovieScene properties are preserved. An unchanged workspace is
rejected as having no explicit changes. This root-property path does not
establish support for arbitrary timeline structure edits. Existing non-float
tracks do not block root-property edits; an AudioTrack case has been verified.
Ordinary directory editing supports existing String, Bool, Integer, Byte, Float, Double, Color,
FloatVector, DoubleVector and supported Transform scalar channels.
ObjectPath channels also support the reference contract described below.
`track_kind` reports the native class name and must remain unchanged; it is not a
creation selector. Eligibility uses the actual supported native channel proxy,
not a fixed track-class list. Unknown channels, channel overrides and unsupported
constraint contexts are rejected rather than omitted.
Root scalar tracks use a readable `root_tracks` map; each binding has a `tracks`
map. Existing tracks support `display_name` edits, deletion by removing an entry,
and clearing with an empty map. Readable aliases remain unique even when duplicate
display names collide with a literal numeric suffix. Captured track identity stays
hidden and is separate from its display name. Query `timeline.track_candidates`
for the target context, then add a track entry with its returned `name` (and `select`
only if the name is ambiguous). Import the new track without `sections`, read its
refreshed native section/channel template, and edit sections in the next import.
The old Float/class initialization list is not a creation catalog for workspaces.
Candidate discovery does not execute section factories. Bound property candidates
use the native exact track type or the property's `SequencerTrackClass` override;
shared C++ ancestry alone does not establish a compatible property animator.
Section templates retain the created track's native context. An omitted new section
range uses its native factory range, not the sequence playback range; specify
`range` for the intended span. Explicit empty ranges follow native reload cleanup.
Each track contains a readable `sections` map; each section contains its
categorized `properties`, using the common reflected `type`/`value` contract.
Track records likewise expose `properties`; recursive sparse edits preserve
omitted native fields. `object_property_definitions` links each track, section
and native section template to centralized definitions. `DisplayName` is excluded
from reflected properties: rename only through `display_name`. `SectionRange`
likewise stays under `range`, and native channels and type context have no second
reflected write path. Float verification covers evaluation options, track tint,
section active state, native new-section defaults, independent reload and late
failure recovery, preserving unrelated properties, channels and real bindings.
Existing track Group Conditions support sparse Operator edits and explicit null
clear, with readable enum definitions, nested condition preservation, independent
reload and late-failure recovery of the complete owned graph. Creating a condition
from a name on an empty single-owned field remains unsupported by the current
array-only candidate context; fixture initialization is not public creation evidence.
Nested owned `properties` use the same category/property-name `type`/`value` rows
as outer properties. Central definition references resolve within the ordinary
definitions file; hidden identity files are not needed to interpret these rows.
Fixed structs with no actionable
members, including the current easing/timecode shapes, are omitted through the
shared property projection; their native state is preserved. Empty growable
containers and selectable null owned references remain visible. This does not
establish editing native details controls outside the reflected field contract.
Each section also contains its
`range` and a `channels` map keyed by native readable names. Each channel contains
a `keys` map. Channels are fixed native members: adding or removing channel names
is rejected before writing. Edit or clear the keys inside the chosen channel.
The collection references `channel_definitions` and `key_definitions` indexed by readable track and channel
name, and `section_definitions` indexed by readable track name. Resolve the selected
key or section definition. The section template contains the
complete native channel set with empty keys, with identical templates deduplicated.
Add a map entry
using that template, remove an existing entry to delete it, or use an empty map
to clear the channel. Omitted record fields are preserved, including nested
`time` and `rate` members. Machine key identities and clock identity stay hidden.
Import preserves existing key values, interpolation, tangents and binding GUIDs;
the native channel orders keys by time. Value edits, moves across a neighboring
key, add/remove, and public clear followed by reload and new addition have been
verified through independent disk reload and fresh export. Section add/remove,
range updates, and clear followed by reload and new addition use the same
ordinary collection contract. Existing sections keep their full channel state,
and unrelated tracks and binding GUIDs are preserved. Omitted section fields
retain captured values; new sections use the exported template and inherited
clock defaults. Section, channel and key identities stay hidden, and separate per-section
or per-key files are no longer parallel write surfaces. Separate per-track structure
files have likewise exited in favor of the enclosing track collection. Ordinary
`section.keys` has exited; both single- and multi-channel sections use
`section.channels.<name>.keys`.
Section bounds support `open`, `inclusive` and `exclusive` independently on
each side. An open bound omits `point`; changing its kind to open removes the
captured point, while explicitly including a contradictory point is rejected.
Changing back to a bounded kind uses the exported template and inherited clock
defaults, so a sparse point can provide just its tick. Missing fields in an
unchanged branch retain their existing values. The shared section definition
lists the allowed kinds and required members.
Native-menu-created infinite Float sections support ordinary key editing and
range changes through save/reload, preserving the original bound objects in
the same world context. This does not establish saving or restarting that world.
Existing Float track rename/delete/clear has been verified through independent
reload, preserving survivor channels and unrelated bindings, including original
actor/component references in the same preview world. These checks do not establish
other channel types, binding structural edits, arbitrary
collection reordering, or saving/restarting the bound world.
New-track creation uses the separate candidate-based path described above.
If staging or final readback fails, saved root-property updates restore the
prior unsaved in-memory state and the original disk contents separately.
The same recovery has been verified for existing key-value, cross-order
key-time, key add/remove, section add/remove and open-range updates rejected after save and reload, including
owned track and section state. Track rename/deletion recovery also verifies the
desired saved and reloaded state before rejection, then separately restores the
complete dirty in-memory owned state and the different original disk contents.

Float and Double share numeric key capture and handle-preserving mutation.
Double values retain native double precision; capture does not narrow them to
float. The shared ScalarKey/ScalarSection definitions describe editable fields,
while new sections inherit their native type from the captured parent track.
Each numeric key exposes editable interpolation and tangent fields in `properties`,
using the common reflected struct codec and exported property/enum definitions.
`value_lexeme` remains the sole key-value field; `properties.fields.Value` is rejected.
Sparse property edits preserve omitted native fields, including nested tangent
members. New keys use native defaults and the selected channel's exported key
template. This follows native key-details assignment; it does not implicitly run
the separate whole-channel auto-tangent menu operation. Property edits, curve
evaluation changes, invalid-input rejection and post-save/reload recovery have
dedicated numeric-channel verification.

Each numeric channel also exposes `default` and `properties`. Set a default with
`{ is_set: true, value_lexeme: "6.25" }`; clear it with `{ is_set: false }`.
An omitted default preserves its current state. Clearing removes the prior value;
explicitly supplying a value with `is_set: false` is rejected. An unset default is
distinct from zero and does not restore the section's original default. Native
Float/Double precision is retained. Channel `properties` exposes only
`PreInfinityExtrap` and `PostInfinityExtrap` through shared enum definitions;
private key storage and a second default-value property are not editable.
The channel definition describes these branches and native defaults. A mixed
Transform case verifies default set/clear and no-key evaluation, extrapolation
outside both ends of the key range, independent reload and late failure recovery.
Other channels, keys and bindings remain intact; Float native checks also verify
that changing settings preserves the original key handle and complete value.

String channels use `value_type: string` and exact text in `value_lexeme`, including
empty strings, Unicode, quotes and newlines. They share the same key collection,
time, native templates and transaction path. A set empty-string default differs
from an unset default. String channels expose no invented tangent or extrapolation
properties. Mixed String/Float/Bool public editing, native section templates,
independent reload, complete non-target state and real bindings have been verified;
late recovery observes persisted edited text before rejection and restores the
original dirty memory and distinct disk state.

ObjectPath keys use `value_type: object_ref` (or `class_ref` for native class
properties) and `value: { path: "..." }`; `value: { path: null }` is a valid null
key. Resolve the centralized `value_definition` for the allowed class. Do not
also supply `value_lexeme`. Defaults use `{ is_set: true, value: { path: "..." } }`
or `{ is_set: false }`. Native null defaults mean unset, so `true` with a null
reference is rejected. New references use the shared property codec's resolution
and class checks; unchanged references, including existing dangling soft paths,
are preserved without capture loading them. New missing or wrong-class references
are rejected. Native section templates inherit the track's class context.
External asset-reference edits, null keys, time/collections/defaults, independent
reload and late recovery have been verified with complete non-target state and
real bindings preserved. Class-valued tracks also have public directory verification
using registered native classes: valid and wrong-class selection, class metadata,
null keys, defaults and section templates persist through independent reload.
Sequence-owned references and cross-world loading are outside this coverage.

Bool channels use the same key collection, time, default and transaction path.
Their keys use `value_type: boolean` and `value_lexeme: "true"` or `"false"`;
numeric coercion and invented key tangent properties are rejected. A set default
of false differs from an unset default. Resolve the exported `LevelSequenceScalar*`
definitions by their actual readable refs; each native channel supplies its own
key defaults. A mixed Bool/Float asset has verified key edits, movement, deletion,
clear/re-add, default set/clear, native section-template creation, evaluation,
independent reload and late recovery with unrelated state and real bindings intact.
Bool extrapolation follows native UE behavior: Linear returns the endpoint value,
and CycleWithOffset follows its cycle path rather than performing numeric offsets.
Integer channels share the same primitive path with `value_type: integer` and
strict signed int32 decimal lexemes for keys and defaults. Fractions, exponents,
booleans and overflow are rejected before writing. `bInterpolateLinearKeys` is
editable through channel properties; evaluation retains UE's native rounding and
extrapolation behavior. A mixed Integer/Float/Bool case verifies boundaries,
negative values, key collections, default set/clear, interpolation, native section
templates, independent reload and late recovery.

Byte channels without an Enum use integer key/default lexemes in `0..255`.
With an Enum, keys use `value_type: enum` and native enum names in `value_lexeme`;
defaults use the same names. Resolve `value_lexeme_definition` for the shared
enum type and candidates. Unknown names, numeric substitutes and out-of-range
bytes are rejected. Existing unnamed values export an empty name: leaving it
unchanged or omitted preserves the captured native value, while choosing a valid
name replaces it. New values cannot use that empty preservation form. If native
zero is unnamed, `value_lexeme_requires_selection` marks the key template as
requiring an explicit enum selection. New sections inherit the track's Enum;
editing does not replace its enum type. Named selection, unnamed old-value
preservation, byte bounds, defaults, native templates, independent reload and
late recovery have been verified. The enum fixture uses a registered native Enum;
Sequence-owned embedded enum objects are not covered.
Color and Vector tracks reuse the same Float/Double value, key, default and
transaction path. New Vector sections inherit the captured track's actual 2–4
channel dimension; missing or invalid dimension context is rejected before native
section creation. Color uses its native four-channel layout. Mixed Color,
two-channel FloatVector and three-channel DoubleVector edits and section templates
have verified complete unaffected channels, real bindings and independent reload;
Vector late recovery observes the edited clean reloaded state before rejection
and restores dirty memory and the distinct original disk state. This does not
establish arbitrary property-track support or validate every candidate-based
track creation context.
A mixed Float/Double asset has verified Double value/time changes, key deletion
and addition, clear/re-add, and new Double sections through active export/import
and independent reload. Existing interpolation, tangents, defaults, non-target
Float/Double state and real bindings in the same world remain intact. Double late
failure recovery observes the saved change before rejection and restores dirty
memory and original disk separately.

Transform sections use the same proxy enumeration and numeric mutation path for
nine named Double channels and the Float Weight channel. A mixed asset has
verified selected-channel value/time and key collection edits, clear/re-add,
creation directly from the exported section template, independent reload and late
failure recovery. The other channels, full tangents/defaults, TransformMask,
non-target objects and real bindings are preserved. Constraint channels, channel
overrides and unsupported value types are not silently skipped. These Transform
editing checks do not establish new-track creation; creation uses the separate
candidate-based path described above.

The thin CLI uses the same current command contract. Use command discovery and
usage for its invocation details; there is no Python-side Sequence/HJSON
business implementation.
