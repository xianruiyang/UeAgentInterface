# T07 Unified HJSON Model Contract

## Scope

The production path shares HJSON parsing, native field descriptions, value codecs,
semantic comparison, source diagnostics and native operation owners. A complete
`FAuthoringModel` is an output/structure view, not admission for an ordinary
property edit. Value-only directory imports use indexed admitted source rows and
merged edit values; complete graph compilation and export fingerprints are
created only for a consumer that requires the complete view.

The former `FUnifiedHjsonSemanticModel`, `FUnifiedHjsonWorkspace`, unified field
registry and edit-bundle/selection wrappers had no production caller in Plugin
Source. They and their dedicated fixtures were retired with the purpose repair;
the twelve composition slots are no longer an import or initialization
requirement. This does not remove the actual source, field, native operation,
committed export or ordinary incremental/full capabilities.

An item whose capture provides a value definition publishes a read-only
`unit.definition` reference to the shared `definitions.hjson` catalog. The common
composer interns that definition and its dependencies without changing the item
identity or placing definition metadata in `unit.values`. Follow its field
definitions to interpret editable values; preserve the exported reference during
import. Items without a captured definition keep their existing representation.

Ordinary property rows keep the native value `type` separate from an optional
read-only `definition` key in their owning catalog. Nested definition references
and item `unit.definition` use the catalog entry's public `ref`; these are not
native types. See [the property-row contract](../hjson-authoring/02_CommonInputValue.md#semantic-workspace-category-grouping)
for catalog and enum-candidate lookup. Capture, projection and import preserve
the same type/definition distinction for root and nested properties.

## Field authority and intent

Field authority is the reflected native field/codec or the registered domain
owner for a real structural operation. Typed authoring intent is extracted once
from the source against its accepted baseline. Omitted fields preserve current
native members; omission is not deletion. Null/default/removal support follows
the actual native type and operation contract, rather than an unused second
field-intent registry. Errors retain the original source and field.

## Reflected editable values

The common editable-property capture separates editable values from complete
native state. An ordinary reflected struct can expose its editable members
while containing read-only members. `InstancedStruct` applies the same rule to
its actual dynamic type. Authoring qualification recursively checks the editable
members, containers and actual dynamic values through the common codec.
An unsupported, noneditable member does not prevent editing other supported
members: initialized native storage retains that member without serializing it
as an editable value.

Read-only members are omitted from the editable value. Common property apply
preserves omitted members of existing values and uses native defaults for new
container elements or a changed dynamic struct type. Explicit read-only writes
are rejected, including writes through a read-only parent. Opaque native members
are preserved by the original storage and collection identity; the editable
projection is not a replacement for that baseline.

Blueprint pin capture accepts Unreal's empty scalar default representation,
including pins whose type was resolved by a promotable operator. It projects
empty real/integer/bool defaults as zero/zero/false without modifying the native
pin. Nonempty malformed numeric values remain diagnostic errors.

Workspace consumers of compact values enable the common completion mode at
their actual property-apply boundary. The independent typed apply contract
remains strict by default. This common value contract does not establish that
every domain has connected its root and nested property paths.

Definition-owned leaf values retain their complete business shape during
compact projection. Container elements carry their own value definitions through
the common codec and projection; a curve channel named `value` is not a generic
wrapper to remove. Definitions follow the captured elements rather than being
inferred from the first element or a default instance.
`InstancedStruct` carries definitions from its actual editable fields, so
heterogeneous array elements can retain different value contracts. A struct's
`fields` object remains a member map during projection: native names such as
`Value` are preserved while their encoded values are compacted.
The same definition tree follows fixed arrays, sets, map keys and values,
set optional values, and actual property-bag fields. Empty containers and unset
optionals do not synthesize leaf definitions from default instances.

FText leaves use the built-in `text` value definition. Ordinary values expose
the source string; generated localization identity stays in canonical/native
state. The same typed definition follows recursive values, without treating
business fields named `localized` as generic wrappers. See
[the text source and identity contract](../UObjectPropertyValueV2.md).

## Source and complete-view consumers

The source resolver retains stable unit identity, original provenance, baseline
and explicit edited-unit bindings. Rebase consumes current facts only for those
edits and their real dependencies; its property result is an overlay over the
admitted source rows. A complete view remains available on demand to actual
structure/export consumers. Native after observation follows the domain's real
scope. Observation differences/inability are public warnings and pending
synchronization, not automatic native rollback or whole-world attribution.
