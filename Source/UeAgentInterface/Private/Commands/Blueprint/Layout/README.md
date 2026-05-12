# Blueprint Auto Layout Module Boundaries

This directory keeps Blueprint auto layout decisions separate from UE command adaptation.

## Public Boundary

- `UeAgentBlueprintAutoLayoutAlgorithm.h`
  - Stable data contract used by `blueprint_layout_graph`.
  - No Unreal graph mutation, no Slate calls, no asset IO.
- `UeAgentBlueprintAutoLayoutAlgorithm.cpp`
  - Public entry points and final self-test registration.
  - Includes ordered internal modules only.

## Internal Modules

- `Private/UeAgentBlueprintAutoLayoutAlgorithm_InternalCore.inl`
  - Internal structs, geometry helpers, connection alignment shortening, reroute point finalization, metrics.
- `Private/UeAgentBlueprintAutoLayoutAlgorithm_Islands.inl`
  - Cycle cutting, implicit fanout edges, weak islands, input-chain detection, node rank and column ordering.
- `Private/UeAgentBlueprintAutoLayoutAlgorithm_IslandPlacement.inl`
  - Dense/anchored column placement, input-chain placement, island-local node placement.
- `Private/UeAgentBlueprintAutoLayoutAlgorithm_InputModel.inl`
  - Target input entry candidates, internal bus groups, boundary entries, and island boundary bus model.
- `Private/UeAgentBlueprintAutoLayoutAlgorithm_FrozenIslandSolver.inl`
  - Frozen-island endpoint selection, placement proposals, bus contracts, and final corridor route construction.

## Change Rules

- Do not add layout decisions to `UeAgentHttpServer_Blueprint_GraphLayout.inl`; that file may adapt UE data, call this algorithm, and write results back only.
- A change to one module must add or update a focused `RunSelfTest()` case for that module's contract.
- Cross-module behavior must be expressed through `FRunResult.DesignDiagnostics`, `FReroutePlan`, or explicit internal structs, not through hidden side effects.
- GraphEditor view, real Slate geometry capture, screenshot, and asset dirty state are outside this algorithm boundary.
