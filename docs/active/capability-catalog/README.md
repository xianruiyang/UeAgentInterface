# Current UAI capability catalog

This directory is the maintained semantic and delivery overlay for the active
UAI surface. It answers which user capability unit owns a current command,
which UE objects and authoring surfaces the unit covers, how far current
evidence actually reaches, and which task must close the remaining vertical
chain.

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
347 exact command identities. T35 through T43 provide representative current UE
verticals across the registered domains, but they do not prove every command ×
object/type × surface × operation × lifecycle cell. Uncovered cells,
distribution and real-content quality remain explicit in each unit's
`not_proven` list and delivery chain.

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
mechanics existed. T41 restored the required capability through the current
`mesh.voxel_grid.session` and `mesh.voxel_grid.apply` contracts and public SDK
product evidence. The four old identities remain absent; the catalog records a
non-exact capability resolution and does not restore their split dispatch.

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
   historical source identity, high-value recovery units and next-task routing
   are current blocking contracts; product readiness remains a vertical evidence
   judgment, not a catalog test side effect.
