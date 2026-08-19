# T14 Blueprint Surfaces

`client.content_graph.import_workspace(...)` is the only public Blueprint HJSON mutation
ingress. Its `source` union accepts `inline`, `single_file`, and `directory`
carriers; no Blueprint-specific apply command is public. Shared
`content_graph.*` reads and `content_graph.export_workspace` are the public
inspection and export paths. Discover the live contract with
`command_search` and `command_detail`; `uai-cli` exposes the same discovery
path.

The accepted entry document is `uai.blueprint.edit/1` with exact root type
`/Script/Engine.Blueprint`. The Blueprint Surfaces provider owns Blueprint
settings, parent class, implemented interfaces, class defaults, members,
component hierarchy, and all managed Blueprint graphs, nodes, pins, and edges.
Blueprint-root content-graph reads resolve only to this provider. The T11 node
and T12 component providers retain only their exact import schemas and do not
compete for Blueprint-root read authority.

T13 is the sole carrier-loading and common recursive initialization authority.
It validates and recursively loads the public source, resolves one entry file,
binds the exact target identity, and passes a normalized `kind: "resolved"`
source containing the workspace documents to T14. T14 rejects legacy top-level
carrier or document fields, selects exactly one document matching `entry_file`,
parses only that normalized entry, and registers only the Blueprint root plus
its owned interface, member, component, graph, node, pin, and edge
initialization contracts. Parsing, routing, recursion, defaulting, reference
binding, rollback, and carrier cleanup remain shared responsibilities.

Create uses `expected_revision: "missing"`; update and upsert use the exact
latest public content-graph revision. Success is terminal only after native
Blueprint compile and save, package unload, reload, and independent readback of
the intended settings, parent, interfaces, defaults, members, component tree,
and managed graphs. Unsupported roots or creation roles, identity mismatch,
duplicate or missing entry documents, stale revisions, unresolved
class/interface identities, compile, save, unload, reload, or readback failures
are typed terminal failures with no retired-owner fallback or residue.
