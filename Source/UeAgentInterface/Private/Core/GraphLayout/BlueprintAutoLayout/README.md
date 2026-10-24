# Blueprint Auto Layout Module Boundaries

This directory keeps Blueprint auto layout split into three explicit stages:
preprocess, core algorithm, and postprocess.

## Stage Directories

- `Preprocess/`
  - Converts UE graph state into the stable layout contract.
  - Owns Blueprint graph target discovery, visible pin filtering, reroute collapse, real Slate node size capture, and pin anchor capture.
  - Pin anchor capture follows UE GraphEditor semantics: Y uses `SGraphPin::GetNodeOffset().Y`; if minimized automation leaves that cache at 0 before widget tick, the same-round arranged pin geometry is used as the fallback row center. X uses the same endpoint compensation as `FConnectionDrawingPolicy::DrawSplineWithArrow`.
- `Core/`
  - Stable data contract used by `UeAgentUnifiedGraphLayoutService`.
  - No Unreal graph mutation, no Slate calls, no asset IO.
  - Public layout entry points.
  - Includes ordered internal modules only.
- `Postprocess/`
  - Writes layout results back into UE graphs.
  - Owns node position writes, reroute node replacement/creation, report restore parsing, and reroute plan JSON serialization.

## Core Internal Modules

- `Core/Private/UeAgentBlueprintAutoLayoutAlgorithm_InternalCore.inl`
  - Internal structs, geometry helpers, connection alignment shortening, reroute point finalization, metrics.
- `Core/Private/UeAgentBlueprintAutoLayoutAlgorithm_Islands.inl`
  - Cycle cutting, implicit fanout edges, weak islands, input-chain detection, node rank and column ordering.
- `Core/Private/UeAgentBlueprintAutoLayoutAlgorithm_IslandPlacement.inl`
  - Dense/anchored column placement, input-chain placement, island-local node placement.
- `Core/Private/UeAgentBlueprintAutoLayoutAlgorithm_InputModel.inl`
  - Target input entry candidates, internal bus groups, boundary entries, and island boundary bus model.
- `Core/Private/UeAgentBlueprintAutoLayoutAlgorithm_FrozenIslandSolver.inl`
  - Frozen-island endpoint selection, placement proposals, inter-island bus planning, and final route construction.

## Change Rules

- Keep command parsing and response construction outside the algorithm. `../UeAgentUnifiedGraphLayoutService.cpp` owns the active orchestration of graph discovery, preprocessing, layout, and application.
- Do not add UE graph mutation to `Preprocess/` or `Core/`.
- Do not add Slate, asset IO, command parameter parsing, or report formatting to `Core/`.
- Do not add new node-to-`FNode` conversion logic outside `Preprocess/`.
- Cross-module behavior must be expressed through `FRunResult.DesignDiagnostics`, `FReroutePlan`, or explicit internal structs, not through hidden side effects.
- GraphEditor view, real Slate geometry capture, screenshot, and asset dirty state are outside this algorithm boundary.
