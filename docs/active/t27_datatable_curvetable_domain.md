# T27 DataTable/CurveTable HJSON domain

Obtain `workspace = client.content_graph.export_workspace(...)`, edit its
ordinary HJSON files and call `workspace.import_changes()`. Existing DataTable and CurveTable updates use the
common semantic workspace, source loader, composition and hidden identity
mapping. Edit the ordinary HJSON files; callers do not recalculate manifest
hashes. Current parameters are discoverable through command search and usage.

DataTable rows form a keyed `rows` collection. Each key is the row name; a new
row does not need a creation candidate because the table already defines its
RowStruct. Editable fields use the shared `type` and `value` representation.
The read-only row schema supplies field types, defaults and enum definitions.
Omitted row properties retain their captured values; removing a row from the
complete collection requests deletion. The table's RowStruct is not changed
by row editing.

The DataTable adapter supplies native row mechanics and consumes the common
recursive property and transaction owners. Preparation uses isolated scratch
objects. Commit assigns owned objects to the real table, and final persisted
readback occurs before transaction commit. Recovery preserves the pre-import
in-memory values, original disk contents and dirty state separately, including
failures after unload and reload. The focused owned-row lifecycle test verifies
these boundaries; it does not establish all DataTable operations.

The existing-table export/edit/import path supports independent readback:
`content_graph.structure` supplies the row schema, and `content_graph.items`
supplies the named rows and their typed values. Re-export uses the same common
HJSON projection. Numeric spelling may be integer or decimal; the schema and
the property's type define the value contract.

Editable root UObject properties appear in the shared `categories` projection.
Root properties and rows can change in one import and one transaction; a
property-only edit preserves the existing row and owned-object allocations before
persistent reload. Root properties use the common reflection apply lifecycle,
including early and post-reload failure recovery. RowStruct remains read-only.

For a missing DataTable target, import an inline or single-file initialization
document with `schema: "uai.datatable_curvetable.edit/1"` and a `root` object.
Set `root.type` to `/Script/Engine.DataTable` and `root.row_struct_ref` to an
existing row struct accepted by the native DataTable editor: a non-transient
native `FTableRowBase` descendant or a user-defined Blueprint struct. The public source loader
supplies the missing-target revision context.

Initialization uses the same keyed `rows` and typed `properties` as ordinary
row editing, and the same `categories` / property-name / `type,value` shape for
root properties. Do not use the former `rows[].key/values` array. Row types and
defaults come from the selected native struct; callers do not supply a hidden
manifest or copy a `row_schema`. Omitted rows initialize an empty table, and
omitted properties retain native defaults. RowStruct is selected at creation
and remains read-only afterward.

Creation now enters the common apply and persistence lifecycle. Public creation
with typed rows, root properties and persisted re-export is verified. Invalid
row values are rejected before creation; failures after staging or persisted
readback restore object, registry and disk absence. The existing-table public
row/property update path also passes after this shared integration.

Existing CurveTable rows use the same keyed collection. Each row's `properties.Curve`
contains `type: curve` and a curve `value`; use the exported row schema for its
fields and enums. Simple curves preserve their global `interp_mode`,
`pre_infinity_extrap`, `post_infinity_extrap`, channel `default_value` and keys.
The scalar input is not assumed to represent seconds. Editing does not implicitly
convert the native table between Simple and Rich modes.

A Simple table's public export/edit/import, persisted native readback and typed
re-export are verified. Invalid default types and mode changes reject before
mutation. Failures after staging and after persisted readback restore the dirty
native curve and original disk bytes separately. Rich curves use per-key
interpolation, tangent mode and tangent weights. The row schema includes key
defaults and enum definitions; ordinary numeric metadata describes native float
storage. Cubic User/Break hand-authored tangents and weights pass public editing,
persisted readback and re-export. Invalid tangent types reject before mutation;
late failure restores complete dirty Rich values and original disk separately.
Native automatic tangent rules remain in effect.

Removing the last row preserves an already selected Simple or Rich mode; the
empty collection can be re-exported and populated again in that mode. A native
Empty table exports `table_curve_mode: empty` and mode templates in its read-only
row schema. Select `simple` or `rich` when adding its first row. An empty collection
with mode `empty` preserves the native Empty state; selecting a typed mode without
a row does not create a synthetic curve. Existing typed tables cannot change mode
through this field. Public removal to typed-empty and re-addition, and native Empty
to a first Simple row, have passed persisted readback and re-export. Invalid mode
and row combinations reject before mutation. An enabled default cannot use the
native `MAX_flt` unset sentinel. Failures after staging and after persisted Rich
readback restore dirty native Empty independently of the original typed disk.

For a missing CurveTable target, use the same initialization schema with
`root.type: "/Script/Engine.CurveTable"`, `root.table_curve_mode` and keyed `rows`.
The typed curve values use the same shape as ordinary editing. Omit both mode and
rows to request native Empty; a nonempty collection requires `simple` or `rich`.
The native CurveTable class has no editable root UProperties; its read-only import
metadata is not made writable through `categories`.

Public Simple creation, native defaults and full curve values, persisted readback
and typed re-export are verified. Public default Empty creation is also verified.
Invalid typed creation input rejects before mutation; failures after staging and
persisted readback restore object, registry and disk absence. The former array initialization, verified-file
export and independent save/reload paths have been replaced by the common source,
workspace and transaction owners.
