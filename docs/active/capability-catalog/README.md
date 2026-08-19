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
highest layer directly supported by current evidence. The initial T34 catalog
can claim activation and public-contract alignment for the 342 exact command
identities, but it intentionally leaves real UE mechanics, applicable
compile/settle and persistence, independent readback, failure cleanup,
distribution and content quality to the referenced vertical task.

Historical `exact_command_set` and `architecture_contract` values are retained
under `candidate_successor`; they do not become current exact successors until
the owning vertical task proves purpose, input, output and product equivalence.
The 202 historically deprecated identities remain explicit non-support, while
the single public self-test retirement remains a retirement decision. All still
require the physical legacy-surface exit audit.

CubeGrid is intentionally represented outside the 939-row inventory. Git
history proves that four voxel-grid commands, persistent sidecar/revision
mechanics and UE smoke tests existed and were removed by the recovery-baseline
commit. The catalog therefore records a missing capability contract; it does not
restore the split legacy implementation or claim current support.

## Maintenance workflow

1. Change command identity or parameters only in the frozen contribution and
   regenerate the formal manifest/binding through their existing owner.
2. Update the affected capability unit in `catalog.json`; every manifest command
   must appear in exactly one unit.
3. When historical semantics are resolved, update the matching row in
   `historical-decisions.json`; never edit the old inventory to change a current
   decision.
4. Advance `highest_verified_layer` only with direct evidence for the exact
   command × object/type × surface × operation × lifecycle slice.
5. Run the CMD capability-catalog tests. Schema, manifest hash/set, uniqueness,
   historical source identity, high-value recovery units and next-task routing
   are current blocking contracts; product readiness remains a vertical evidence
   judgment, not a catalog test side effect.
