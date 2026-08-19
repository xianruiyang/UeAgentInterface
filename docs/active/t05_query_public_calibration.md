# T05 Query public calibration

## Active boundary

The current `/api/uai/command` route activates the one process-wide immutable
registry composed by T35. T05 contributes these three commands through
`query-calibration-command-owner`; it does not freeze or publish a private
registry:

| command | contract hash | responsibility |
|---|---|---|
| `query.command.search` | `sha256:5e625c558e8ada9eb4e18b1491d31c76c2f4335c52120345ed483e89563787d8` | Rank a bounded set of command candidates from intent and explain each match. |
| `query.command.detail` | `sha256:78a228528dc0a42590fa51f3b398246448ca7e74a20f7d44442f703a12870022` | Return the complete purpose, field contract, invocation templates and stable tutorial references for one exact candidate identity. |
| `query.calibration.read` | `sha256:3bac8ef382dcd4d6a4c631d13de0ea918569f4cb70fefee3ef08de16c30d1349` | Read one representative real UE query source with bounded paging and explicit completeness. |

Search is fuzzy only while producing candidates. Detail and execution require the returned `command_id` and `contract_hash` unchanged. Unknown or stale identities never fall back to prefixes, aliases, another provider, or an SDK mapping.

## Query source contract

`query.calibration.read` accepts one exact category:

- `capability`: current engine and Editor-world availability;
- `content_search`: current `/Game` Asset Registry identities;
- `candidate_catalog`: exact loaded actor-class paths in the current Editor world;
- `state`: current world identity, actor count, package dirty state and world type;
- `reference`: current world, persistent level and world-settings identities;
- `runtime_probe`: current Editor process, game-thread, world, PIE and engine availability.

Every successful result contains `source_id`, `state_layer`, `revision`, `read_only`, `completeness`, `total_items`, `next_cursor`, `missing_sources` and bounded `items`. `completeness` is one of `complete`, `paged`, `partial` or `degraded`; a missing source is not promoted to complete. Limits are `1..8` for command search and `1..100` for source reads.

All three commands require the authenticated transport principal and `uai:query` authority. The production source rejects non-game-thread reads. A provider reporting non-read-only behavior is rejected before a successful result.

## Ownership and inactive legacy path

The active chain is HTTP transport → `FUeAgentCommandService` → process exact
ingress → `FUeAgentExactCommandDispatch` → one of the three T05 handlers → the
same final registry or production Query source. The module no longer installs
`MakeUnavailableCommandIngress()` and does not activate the legacy aggregate
`FUeAgentQueryServiceComposition`. Older Query providers remain source
inventory for T06 only and are not reachable through the active process
registry.

T05 does not claim that every historical Query command has migrated. Its production claim is limited to command discovery plus the six calibrated representative source categories above.
