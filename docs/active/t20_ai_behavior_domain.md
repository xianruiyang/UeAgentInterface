# T20 AI Behavior HJSON Domain

`ai-behavior-domain-owner` is the only public owner for `ai.behavior.candidates` and the T20 HJSON domain projection. The domain provider owns only classification, initialization-contract selection, schema-to-command routing, and domain completion checks.

The public SDK calls are `client.ai.behavior_candidates(...)` and
`client.content_graph.import_workspace(...)`. Use
`client.command_search(...)` and `client.command_detail(...)` to discover and
inspect their live contracts; `uai-cli` exposes the same discovery path. No
task-specific helper or tutorial-owned registry is authoritative. T13 is the
sole common recursive initialization authority; T20 contributes only the AI
Behavior initialization contracts and consumes that seam. T13 is also the
sole inline, single-file, and directory carrier loader and recursion owner;
T20 consumes only its normalized `source.kind=resolved` document set, selects
exactly the declared `entry_file`, and rejects legacy top-level `carrier` and
`hjson_document` fields.

The supported root identities are exact `BehaviorTree`, `BlackboardData`, and `StateTree` assets. Their native writes delegate unchanged to `ai-command-owner` through `ai.behavior_tree.graph.apply`, `ai.blackboard.schema.apply`, and `ai.state_tree.hierarchy.apply`. The domain does not duplicate GraphCompound planning, recursive initialization, UE node construction, compile, save, reload, readback, or cleanup mechanics.

`ai.behavior.candidates` is read-only and target-sensitive. For a BehaviorTree it returns concrete BT node, service, and decorator classes; for a StateTree it returns concrete Blueprint node classes and native task, condition, and evaluator structs; for BlackboardData it returns concrete Blackboard key types. `name` and `binding_revision` are exact opaque identities copied unchanged into the matching HJSON creation entry.

The schema is `uai.ai_behavior.edit/1`. Existing-root import requires the current revision and one exact root-appropriate operation. BehaviorTree graph edits use stable `node:<guid>` identities, `item` entries for composites, tasks, services, and decorators, `relation` entries for graph connections, and the single `binding:blackboard_asset` binding. Blackboard schema operations consume exact key-type candidates. StateTree node entries use `node:<guid>` with an exact owner-state identity, node class or struct identity, and placement (`global_task`, `evaluator`, `state_task`, or `state_condition`).

All three roots use the shared `ContentGraphRead` authority. Its revision includes BehaviorTree topology, subnode attachment, Blackboard binding and entries, and StateTree hierarchy and node placement. Successful import must compile or validate through the domain owner, save, reload, and return this independent readback. A missing root, stale revision, mismatched target, invalid candidate, unsupported root, or unregistered operation fails before write or rolls back without residue. Export remains explicitly unsupported until a separate complete-workspace contract is registered.
