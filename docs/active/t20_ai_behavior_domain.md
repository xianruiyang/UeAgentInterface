# T20 AI Behavior HJSON domain

`ai-behavior-domain-owner` is the sole public owner for
`ai.behavior.candidates` and the `uai.ai_behavior.edit/1` domain. The public SDK
calls are `client.ai.behavior_candidates(...)` and
`client.content_graph.import_workspace(...)`. Inspect their live contracts with
`client.query.command_search(...)` and `client.query.command_detail(...)`; `uai-cli exec`
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

`ai.behavior.candidates` is read-only and target-sensitive. Its public filters
are `target`, optional text `query`, optional exact `id`, `limit`, and `cursor`.
The BehaviorTree branch enumerates the target's native blank-background schema
menu without opening an asset editor. It reports the complete search surface and
separately reports executable creation coverage through `coverage_scope`,
`coverage_complete`, `creation_coverage_complete`, `native_action_count`, and
`executable_action_count`.

For BehaviorTree, composite and task menu actions are directly creatable. Auto
Arrange and Add Comment remain searchable but explicitly report `search_only`;
service/decorator attachment and non-blank pin or selection contexts are not
claimed by this contract. Put only the selected `candidate.name` or
`candidate.id` inside the candidate object. Node identity, position, ownership,
and relations remain in the surrounding edit:

```hjson
{
  identity: "node:11111111-2222-3333-4444-555555555555"
  element_kind: item
  intent: create
  depends_on: []
  value: {
    candidate: { id: "candidate:..." }
    position: [640, 192]
  }
}
```

When both `name` and `id` are supplied they are conjunctive assertions. The
domain resolves the selector during normalization, derives or checks the
carrier and payload types, then re-enumerates the same native context immediately
before commit and invokes the same schema action. BehaviorTree relations use the
native schema connection path; the root must connect to a composite, not
directly to a task.

Blackboard, EQS, StateTree, and SmartObject candidate results currently retain
their legacy full-binding compatibility contract. Their class/struct catalogs
must not be described as native editor-menu equivalence until each owning
context is migrated and independently verified.

## Lifecycle and readback

Success requires the native owner to finish mutation, compile or domain
validation, save, unload/reload, and independent `ContentGraphRead` capture. The
returned snapshot and `reload:` persistence receipt cover the persisted root;
command success alone is insufficient. Unsupported roots or operations, stale
revisions, mismatched targets, invalid candidates, and lifecycle failures fail
without a legacy fallback. `content_graph.export_workspace` remains explicitly
unavailable until a complete round-trip export contract is registered.
