# Dormant command lifecycle and inventory

This development-only document records the centralized compatibility surface
for commands whose normal authoring responsibility has moved to an active HJSON
owner. It is not distributed in the `ue-agent-interface` skill.

## Lifecycle contract

Dormancy is one registry lifecycle state, not physical retirement and not a
per-command implementation technique:

- the original public command ID, frozen contract, unique C++ owner and exact
  handler remain registered and executable;
- normal `query.command.search` excludes dormant commands;
- `query.command.search(..., mode="compatibility")` includes active and dormant
  candidates and labels every candidate with `lifecycle_status`;
- a dormant candidate and its detail expose `successor_command_ids`,
  `lifecycle_reason`, and `requires_user_confirmation=true`;
- generated named SDK methods are emitted only for active commands;
- `Client.execute` and `Client.execute_exact` reject a known dormant command;
- after explaining why active successors are insufficient and obtaining the
  user's explicit agreement, development code may call
  `Client.execute_dormant(..., user_confirmed=True)` or pass both an exact
  contract hash and `--allow-dormant` to `uai-cli exec`;
- disabled commands remain undiscoverable and non-executable.

The single data owner is
`Core/Contract/Command/UeAgentCommandLifecyclePolicy.cpp`. Adding or changing a
dormant identity updates that table and its shared projections; registrations,
handlers and search code must not carry local dormant lists.

## Current dormant commands

| Dormant command | Preferred active successor | Covered authoring responsibility |
| --- | --- | --- |
| `world.actors` | `content_graph.structure`, `content_graph.items` | Persisted World Actor discovery and selected Actor structure |
| `world.actor_components` | `content_graph.items` | Persisted component structure for selected Actors |
| `world.property` | `content_graph.properties` | Persisted editable reflected Actor and Component properties |
| `world.object_transform.read` | `content_graph.structure`, `content_graph.items` | Persisted Actor and Component transform readback |
| `world.object_transform.update` | `content_graph.import_workspace` | Persisted Actor and Component transform mutation through a revisioned World HJSON edit |

This set does not include `world.actor.create`. HJSON is the preferred structure
authoring owner, but the current evidence does not prove exact equivalence for
that command's candidate contract, inputs, outputs and failure boundary. It
therefore remains active until a separate migration decision is justified.

Historical commands already removed from the current registry are not dormant
and must not be restored merely to populate this compatibility surface.
