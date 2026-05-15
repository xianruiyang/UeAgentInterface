# Postprocess

This stage writes core layout output back into UE graphs.

Responsibilities:

- Apply `FNode.NewX/NewY` to `UEdGraphNode::NodePosX/NodePosY`.
- Remove and recreate reroute nodes when requested.
- Convert route plans into UE reroute links.
- Restore a saved layout report into a model.
- Serialize node and reroute diagnostics for command reports.

No new node-to-`FNode` conversion or layout decisions should be added here.
