# T07 Unified HJSON Model Contract

## Scope

T07 establishes the common HJSON semantic model used after canonical state capture and before export, import, or domain mechanics. It is a module contract only: it does not claim that any asset domain or public command has completed migration.

The only semantic direction is:

`real UE state -> canonical snapshot -> FAuthoringModel -> FUnifiedHjsonSemanticModel -> FUnifiedHjsonWorkspace / FUnifiedHjsonSelectionPackage`

Physical directory, single-file, and inline carriers are projections of the same semantic fingerprint. Token estimates provide telemetry for projection choices.

## Common composition schema

Every unified model carries the same twelve named composition slots: target, property set, member set, structure, item, endpoint, relation, order, layout, temporal contract, spatial contract, and external input. A slot may contain multiple units or be empty, but all source semantic units must have exactly one composition owner. Full-state, minimal-create, and local-edit are intents over this same model; they do not select separate parsers or semantic schemas.

`TryCreateUnifiedHjsonSemanticModel` rejects missing slots, duplicate or unbound units, unknown units, incompatible unit kinds, and target bindings that do not match the core model. Its model fingerprint is independent of authoring intent.

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

`FExportFieldRegistry` remains the single field-owner authority. `FUnifiedHjsonFieldRegistry` adds authoring intent without creating a second field registry. Every exported field has exactly one unified contract.

Omission always resolves to `preserve`. Mutation requires one explicit intent: `set_value`, `set_null`, `restore_default`, or `remove`. Null is allowed only for a declared nullable field; restore-default requires a reproducible default; remove requires presence-sensitive semantics. Unsupported combinations fail before mutation with a typed `hjson_field_*` error.

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

## Workspace index and selection

`FUnifiedHjsonWorkspaceIndex` records every unit, its composition, owner, field-set owner, and direct relations. `TryCreateUnifiedHjsonWorkspace` accepts a layout only when it carries the same core semantic fingerprint.

`TryResolveUnifiedHjsonSelection` delegates owner, controlling-field, placement, and endpoint closure to the existing shared selection owner. It adds direct relation IDs from the resolved semantic units and then reuses that same owner for the final closure; it does not implement a parallel selection algorithm.
