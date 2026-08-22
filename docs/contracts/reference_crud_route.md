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
- commands: `reference.asset_read` and `reference.asset_apply`;
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
| Internal Query descriptor | `uai.query.reference.read` |
| Internal Query descriptor hash | `sha256:ec85d816c17212cf7911440546134a94f86933b230abce221b9300e32c2e7dcf` |
| Internal Operation descriptor | `uai.operation.reference.crud` |
| Internal Operation descriptor hash | `sha256:fbde900ec387e61915a21e97ae5575a85337b3866d4299be961f75a5fc888491` |

These descriptor identities are server-owned mechanics contracts. The public
command identities and binding hashes remain owned by the process-active command
registry and its generated manifest; clients do not submit descriptor IDs or
provider bindings. Unknown public commands are discovered through
`query.command.search` and resolved through `query.command.detail`.

## Query and mutation flow

The caller performs a direct `reference.asset_read` when pre-state is needed,
submits one typed `reference.asset_apply`, and then calls
`reference.asset_read` again for independent persisted readback. The public
read route composes the dedicated `uai.query.reference.read` descriptor. It
does not reuse generic `asset.properties` absence semantics and does not expose
the application-internal catalog, ticket, planning, effect, durable or
verification steps.

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

For delete, independent persisted readback returns business success with a
valid snapshot containing no entry for the target. Other asset Query commands
continue to report an unresolved target as a typed failure. After C8D confirms
the package artifacts are absent, any
still-loaded package for that target is left clean so the user Editor does not
surface the deleted asset as a pending dirty resource. A successful response
alone is never evidence that the asset changed.
