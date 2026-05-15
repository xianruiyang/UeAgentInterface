# Preprocess

This stage converts live UE graph/editor state into the pure layout contract in
`Core/UeAgentBlueprintAutoLayoutAlgorithm.h`.

Responsibilities:

- Resolve Blueprint graph targets.
- Convert `UEdGraphNode` objects into `FNode` records.
- Convert visible pin links into `FEdge` records, collapsing existing reroute nodes when building the semantic graph.
- Capture real node sizes and pin anchors from Slate.
- Keep all UE/Slate-specific measurements outside the core algorithm.

Pin anchor rule:

- Y must come from `SGraphPin::GetNodeOffset().Y`, matching UE's own Straighten Connections implementation. A zero cached offset in minimized automation means the pin has not ticked yet; in that case, use the same-round arranged node/pin geometry fallback instead of accepting 0.
- X must be converted from the actual GraphEditor wire endpoint, including the same start/end fudge and arrow offset used by `FConnectionDrawingPolicy::DrawSplineWithArrow`.
