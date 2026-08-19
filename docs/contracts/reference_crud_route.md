# Reference CRUD command contract

## Scope

The reference CRUD command family is the first active generic-effect composition of the
new UAI framework. It proves one transport-neutral Query/Operation path against
a controlled Unreal asset surface:

- package root: `/Game/UAI/Reference/`;
- object type: `UBlueprint`;
- property: `BlueprintDisplayName`;
- intents: `create`, `update`, `replace`, and `delete`.

It is not a general reflection escape hatch. Other capability families must
register their own candidate source and thin UE hook while reusing the common
contracts.

## Network edge

The current adapter binds the shared public command edge:

- method: `POST`;
- route: `/api/uai/command`;
- wire schema: `uai.command.v1`;
- commands: `reference.catalog_search`, `reference.asset_read`, and
  `reference.asset_apply`;
- authentication header: `X-UeAgentInterface-Token`.

HTTP belongs only to the network adapter. Query, Operation, ticket, planning,
effect, durable, verification, and outcome modules do not depend on HTTP and
can be reused by a future TCP or UDP adapter that satisfies the declared
transport capabilities.

The retired `/api/uai/reference` and `/api/uai/export` transports are not
bound. Semantic export uses the formal `content_graph.export_workspace`
command documented in `read_export_command_contract.md`.

## Canonical identities

| Contract | Identity |
|---|---|
| Query descriptor | `uai.query.reference.read` |
| Query descriptor hash | `sha256:b66bbe297913f4df06f04bec2091211abd48adb7ff89b1b021e59a8495038c75` |
| Operation descriptor | `uai.operation.reference.crud` |
| Operation descriptor hash | `sha256:9f8882546a7f47a2f80b43b038c09eb4e5ea20bf4f52ed65cdc8432d43cfdd12` |
| Shared registry hash | `sha256:5388c801035019359c568de214b7dc2fe2d2979bf429175d89a03932c41253ea` |

The request session uses the live server instance ID, registry epoch `1`, and
the shared registry hash. A catalog candidate advertises the Operation
descriptor; C3Q issues and validates the ticket against that candidate binding.
C3O consumes the same signed ticket without route-local re-signing or skipped
revision checks.

## Query and mutation flow

The caller performs:

1. `catalog_search`;
2. `catalog_resolve`;
3. persisted `state_read`;
4. one typed Operation request;
5. the same three Query steps again for independent readback.

The Operation payload contains only:

- `session`;
- `target.ticket`;
- `target.binding`;
- `intent`;
- `proposed_after`;
- `projection`.

The caller must not submit a plan, cached snapshot, execution receipt, or
claimed readback. The application re-reads persisted pre-state, creates the C6
plan, executes through C7 and GenericEffect, persists through C8D, verifies
through C9 using an independent oracle, and returns the C10 outcome.

`complete` requires durable acknowledgement after independent verification.
Mismatch remains `partial`; unavailable state remains `state_unknown` and is
not automatically replayed. Legal canonical values have no artificial nesting
depth limit; cumulative resource budgets remain authoritative.

## Result and errors

A successful wire response is a typed C0 `QueryResult` or `OperationResult`
whose descriptor identity matches the request. Business refusal is returned as
structured `query_error` or `operation_error`; transport and wire failures stay
in their respective protocol layers.

For delete, independent persisted readback returns a valid snapshot with no
entry for the target. After C8D confirms the package artifacts are absent, any
still-loaded package for that target is left clean so the user Editor does not
surface the deleted asset as a pending dirty resource. A successful response
alone is never evidence that the asset changed.
