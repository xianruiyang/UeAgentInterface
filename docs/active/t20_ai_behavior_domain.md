# T20 AI Behavior HJSON domain

`ai-behavior-domain-owner` is the sole public owner for
`ai.behavior.candidates` and the `uai.ai_behavior.edit/1` domain. The public SDK
calls are `client.ai.behavior_candidates(...)` and
`client.content_graph.import_workspace(...)`. Inspect their live contracts with
`client.command_search(...)` and `client.command_detail(...)`; `uai-cli exec`
forwards the same exact generated binding and is the CLI ingress.

The shared source loader owns inline, single-file, and directory carriers. T20
consumes only the normalized `source.kind=resolved` document set and does not
accept legacy top-level `carrier` or `hjson_document` fields.

T13 remains the sole common recursive initialization authority. T20 registers
only AI Behavior root and owned-entry initialization contracts and consumes
that shared seam; it does not create another recursive dispatcher.

## Supported roots and native owners

| Root type | Create command | Mutation command |
| --- | --- | --- |
| `/Script/AIModule.BehaviorTree` | `ai.behavior_tree.asset.create` | `ai.behavior_tree.graph.apply` |
| `/Script/AIModule.BlackboardData` | `ai.blackboard.asset.create` | `ai.blackboard.schema.apply` |
| `/Script/AIModule.EnvQuery` | `ai.eqs.asset.create` | `ai.eqs.graph.apply` |
| `/Script/StateTreeModule.StateTree` | `ai.state_tree.asset.create` | `ai.state_tree.hierarchy.apply` |
| `/Script/SmartObjectsModule.SmartObjectDefinition` | `ai.smart_object_definition.asset.create` | `ai.smart_object_definition.apply` |

T20 owns root classification, target/revision validation, candidate binding,
and lifecycle completion. It forwards one normalized request to the existing AI
command owner and does not duplicate graph planning, UE construction,
compilation/validation, persistence, reload, readback, or cleanup mechanics.

## Root lifecycle and edit shape

A new root uses `expected_revision: "missing"` and one root `create` object.
The public target owns the package path and asset name; conflicting declarations
fail before write. Creation and edit are deliberately separate commands: create
the root, use the returned independent readback revision, then submit one exact
root `operation` for that root type.

Existing-root import requires the exact current `content_graph` revision. The
operation params use each native command's stable item/relation/key/slot shape:
BehaviorTree nodes and hierarchy, Blackboard entries, EQS options and
generators, StateTree states and node placement, or SmartObject slots and
behavior definitions.

## Candidate-to-authoring chain

`ai.behavior.candidates` is read-only and target-sensitive. It returns concrete
BehaviorTree node/service/decorator classes, Blackboard key types, EQS generator
classes, StateTree task/condition/evaluator structs, and SmartObject behavior
definition classes as applicable. Copy the full candidate object unchanged into
the matching creation entry. The domain verifies its type, placement, contracts,
operation support, target context, and `binding_revision` before translating it
to the native field. Stale, missing, or conflicting candidates fail before
write.

## Lifecycle and readback

Success requires the native owner to finish mutation, compile or domain
validation, save, unload/reload, and independent `ContentGraphRead` capture. The
returned snapshot and `reload:` persistence receipt cover the persisted root;
command success alone is insufficient. Unsupported roots or operations, stale
revisions, mismatched targets, invalid candidates, and lifecycle failures fail
without a legacy fallback. `content_graph.export_workspace` remains explicitly
unavailable until a complete round-trip export contract is registered.
