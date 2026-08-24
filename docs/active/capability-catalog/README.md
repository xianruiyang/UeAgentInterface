# Current UAI capability catalog

This directory is the maintained semantic and delivery overlay for the active
UAI surface. It answers which user capability unit owns a current command,
which UE objects and authoring surfaces the unit covers, how far current
evidence actually reaches, and which gaps or deferred decisions remain. It does
not select or track the next task.

It is deliberately not a runtime registry:

- `Resources/UaiCommandManifest.json` remains the only public command identity,
  parameter and binding contract.
- the frozen `DomainCapability` authority remains the only process activation
  owner.
- `catalog.json` classifies those exact commands for delivery and evidence; the
  runtime never reads it.
- `historical-decisions.json` owns the current disposition overlay for the 939
  pre-refactor identities. The old inventory and approval document are immutable
  source evidence, not active task state or successor authority.

## Evidence semantics

A registered command is not a delivered capability. Every unit records the
highest layer directly supported by current evidence. The current catalog owns
352 exact command identities in 45 capability units. T35 through T43 provide
representative current UE
verticals across the registered domains, T44 proves legacy public/compiled
surface exit, and T45 proves the Plugin/CMD/Pak/installed distribution contract.
T46 additionally proves a maintained seven-Actor whitebox/spatial real-content
slice while preserving and restoring the user's minimized main Editor. These
results still do not prove every command × object/type × surface × operation ×
lifecycle cell or final visual/content quality. T47 executes the current 54
command discovery/candidate/read/reference slice through the public SDK, closes
the persisted Reference CRUD/readback defect, and records exact typed failures.
That same matrix proves that several successful read envelopes do not yet carry
the domain fact selected by their public input. T47A and T47B subsequently close
the applicable read and durable World package semantics. Every unit retains the
direct evidence boundary in `not_proven` and any applicable product decision in
`blockers`; neither field creates an execution task.

T59 closes one transform-candidate visual vertical and its new-map durability,
but explicitly leaves the historical 7E26 journal to T63 rather than generalize
that result. T60 restores and verifies the external UE-KB Hybrid query chain; it
does not add a UAI command identity or advance a catalog unit. T61 proves the
dynamic tutorial/detail projection for the then-current 348 commands. T62 adds
`content_graph.authoring_schema` and `timeline.sequence.settings.update`; its
real product evidence covers the Sequence sparse HJSON export/edit/import path,
not every provider or a separate exact invocation of the settings command. T63
adds `asset_durable.journal.read` and `asset_durable.journal.reconcile`, and
executes their exact public SDK read/reconcile chain for the bound 7E26 terminal
case without replay. It does not prove that every historical journal is
recoverable. Those commands belong to the independent
`CU-DURABLE-MAINTENANCE` unit because their formal owner crosses asset, world,
modeling and configuration consumers and is distinct from both AssetSimple
mutation and `operation.reconcile`.

Dynamic task state, dependencies and `next_action` belong only to the outer
project task table. A new capability task starts from a concrete real UE content
consumer and references the affected catalog unit; catalog gaps are not
projected into command-by-command or carrier-by-mode matrices. This keeps a
distributed semantic catalog usable without copying project-local task state.

Historical `exact_command_set` and `architecture_contract` values are retained
under `candidate_successor`; they do not become current exact successors without
direct purpose, input, output and product equivalence. In the v2 overlay,
`current_disposition` is the final resolution for every row: 736 required
capability contracts, 202 explicit non-support decisions and one public
retirement. A required capability contract is an obligation classification,
not an implementation or exact-successor claim.

The identical public-exit fact is stored once in
`legacy_public_surface_exit`, rather than copied into 939 rows. It records that
all old identities are absent from the current manifest, the compiled legacy
registry and HTTP handler are physically removed, and retained mechanics are a
private implementation boundary consumed only by current domain owners. The
architecture and CMD tests are the executable evidence for that contract.

CubeGrid is intentionally represented outside the 939-row inventory. Git
history proves that four voxel-grid commands and their sidecar/revision
mechanics existed. T41 first restored the required capability through the
current `mesh.voxel_grid.session` and `mesh.voxel_grid.apply` contracts. T50
replaced the active sidecar state with one serialized editor-only Actor
component, verified C8D persistence and C9 isolated-package equivalence through
the public SDK, and retained the old sidecar only as an import-once source moved
to a recoverable tombstone. The four old identities remain absent; the catalog
records a non-exact capability resolution and does not restore their split
dispatch.

## Maintenance workflow

1. Change command identity or parameters only in the frozen contribution and
   regenerate the formal manifest/binding through their existing owner.
2. Update the affected capability unit in `catalog.json`; every manifest command
   must appear in exactly one unit.
3. Change a historical row only when its final disposition changes. Keep
   candidate mappings historical-only, and update the single
   `legacy_public_surface_exit` object when the shared exit mechanism changes;
   never edit the old inventory to change a current decision.
4. Advance `highest_verified_layer` only with direct evidence for the exact
   command × object/type × surface × operation × lifecycle slice.
5. Run the CMD capability-catalog tests. Schema, manifest hash/set, uniqueness,
   historical source identity, high-value recovery units and the physical
   absence of dynamic task-routing fields are current blocking contracts;
   product readiness and future task selection remain vertical evidence
   judgments, not catalog test side effects.
