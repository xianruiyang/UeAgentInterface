# T15 UMG/Widget Domain

`client.content_graph.import_workspace(...)` is the only public Widget
Blueprint HJSON mutation ingress. Its `source` union accepts `inline`,
`single_file`, and `directory` carriers; no UMG-specific apply command is
public. Shared `content_graph.*` reads and export are the public persisted
inspection paths. Discover the live contract with `command_search` and
`command_detail`; `uai-cli` exposes the same discovery path. Existing
non-HJSON UMG asset creation and widget-event discovery commands keep their
separate owners.

The accepted entry document is `uai.umg.edit/1` with exact root type
`/Script/UMGEditor.WidgetBlueprint`. The provider owns Widget Blueprint
settings and members, widget tree, native slot layout, bindings, animations,
recursive properties, and managed Blueprint graphs. Its content-graph readback
identifies widgets, slots, bindings, animations, hierarchy relations,
recursive properties, compile diagnostics, and persisted revision.

T13 is the sole carrier-loading and common recursive initialization authority.
It validates and recursively loads the public source, resolves one entry file,
binds the exact target identity, and passes a normalized `kind: "resolved"`
source containing the workspace documents to T15. T15 rejects legacy top-level
carrier or document fields, selects exactly one document matching `entry_file`,
parses only that entry, and registers only the Widget Blueprint root plus its
owned widget, slot, binding, and animation initialization contracts. It does
not implement a second parser, directory walker, recursion engine, reference
binder, rollback path, or carrier cleanup path.

Create uses `expected_revision: "missing"`; update and upsert use the exact
latest public content-graph revision. Success is terminal only after normal
Widget Blueprint compile and save, package unload, reload, and independent
readback. Unsupported roots or creation roles, source identity or entry
mismatch, stale revisions, unresolved widget/slot classes, invalid hierarchy
or bindings, and compile or lifecycle failures are typed terminal failures
without a retired-owner fallback or residue.
