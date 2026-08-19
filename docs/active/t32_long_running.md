# T32 Long-Running Operation Lifecycle

## Scope and authority

The active `long-running-lifecycle` contribution owns exactly `operation.status`, `operation.receipt`, `operation.cancel`, `operation.resume`, `operation.reconcile`, and `operation.release` through `long-running-session-owner`. It does not own a start command. Compile/build, Editor-session, and capture/media start commands remain assigned to T34 and T44. The exact handlers require `uai:operate` and only adapt one public request to the process-owned `OperationService`; they do not create a second registry, replay a side effect, or fall back to the historical twelve-command composition.

## Identity, disconnect, and reconstruction

Every lifecycle request forwards the complete server-issued `operation_ref`: `operation_id`, `owner_lease_id`, and `server_session_id`. `operation.resume` and `operation.reconcile` additionally require the caller's exact last observed `expected_generation`. A stale generation, wrong lease, or wrong server session fails closed before native work advances. Transport disconnect is not a terminal operation result. A caller reconnects and submits the same reference; it never synthesizes a lease, increments a generation, or automatically repeats the initiating request.

The process host owns one lazy lifecycle port, one server-session identity, one lease secret, and—after the Editor world becomes available—one long-running runtime. Active registry construction can therefore finish during early Editor startup without creating a throwaway runtime. Reconstructing CommandService or its transport reuses the same process host and operation registry. A missing retained state is reported as `state_unknown` where reconciliation evidence permits that conclusion; it never makes replay retry-safe.

## Public SDK and CLI

The six contracts are projected only through the final manifest into the
default `operation` namespace. A caller imports `uai`, connects from TOML and
uses `client.operation.status(operation_ref)`,
`client.operation.resume(operation_ref, expected_generation)`, or
`client.operation.cancel(operation_ref, reason)`. Generic
`client.execute(...)`, exact `client.execute_exact(...)`, and `uai-cli exec`
reuse the same Client, Protocol and Transport; no task-specific Python module
or binding registry remains authoritative.

```text
uai-cli --connection connection.toml exec operation.status --params-json {"operation_ref":{"operation_id":"...","owner_lease_id":"...","server_session_id":"..."}}
```

## Cancellation and cleanup

`operation.cancel` records a stop request, but cancellation becomes terminal only after the native owner reaches a safe boundary, joins the owned work, and releases its exact thread, process, file, or UE resource. `operation.receipt` is terminal-only. `operation.release` removes only a retained terminal operation and never terminates unrelated Editor or user resources. The runtime's generation and lease fences, terminal receipt, state-unknown semantics, join/release behavior, and no-replay rule are the authoritative mechanics behind all six public routes.

## Legacy exit

The historical `Composition/LongRunningSession` code remains a mechanics dependency and T90 migration inventory, not a public fallback authority. Its six start commands are inactive until their assigned tasks, and its lifecycle handlers are not installed by the active exact registry.
