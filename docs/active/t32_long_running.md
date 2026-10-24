# T32 Long-Running Operation Lifecycle

## Scope and authority

The active `long-running-lifecycle` contribution owns `operation.status`, `operation.receipt`, `operation.cancel`, `operation.resume`, `operation.reconcile`, `operation.release`, `operation.discover`, `operation.recover`, and `operation.takeover` through `long-running-session-owner`. Start commands remain assigned to their domain contributions. The exact handlers require `uai:operate` and adapt requests to the process-owned Operation service; they do not create a second registry or replay business work.

## Identity, disconnect, and reconstruction

Every start result exposes a server-issued handle as `start_result.operation`. Use that handle directly, or normalize a result with `client.operation.from_result(start_result)`; callers do not inspect or construct its internal identity. Transport disconnect is not a terminal operation result. Reconnect and query the same handle; never replay the initiating request.

The manifest's `supports_operation_recovery` field comes from the shared recovery
policy over frozen command contracts, including deferred workspace imports.
The SDK journals request identity before submission. The owner records trusted
caller, request, command and subject metadata. Recovery resolves that original
request; discovery lists only the authenticated principal's retained operations
without exposing custody; explicit takeover rotates the lease. A takeover request
can itself recover its reply. Local callers share this principal; it is not an
agent isolation boundary.

The process host owns one lazy lifecycle port, one server-session identity, one lease secret, and—after the Editor world becomes available—one long-running runtime. Active registry construction can therefore finish during early Editor startup without creating a throwaway runtime. Reconstructing CommandService or its transport reuses the same process host and operation registry. A missing retained state is reported as `state_unknown` where reconciliation evidence permits that conclusion; it never makes replay retry-safe.

## Public SDK and CLI

The nine lifecycle contracts are projected only through the final manifest into the
default `operation` namespace. A caller imports `uai`, connects with `uai.connect_project()` and
uses `handle.status()`, `handle.resume()`, or
`handle.cancel(reason)`. Generic
`client.execute(...)`, exact `client.execute_exact(...)`, and `ueagentinterfacecmd exec`
reuse the same Client, Protocol and Transport; no task-specific Python module
or binding registry remains authoritative.

## Cancellation and cleanup

`operation.cancel` records a stop request. The shared server driver observes
started work even after the initiating client exits and performs Join/Release.
It does not start accepted operations. Cancellation becomes terminal after the
native owner reaches its safe boundary and completes cleanup; cancellation before
Start has no native resource to drain. Late resume/cancel returns the retained
terminal snapshot without rerunning native actions.

A failed Start is safely terminal only when its port explicitly confirms that
no native resource needs cleanup. The native editor-session port supplies this
fact for context rejection before native Start. Failures after a native start
attempt retain the conservative unknown-cleanup behavior; mutation diagnostics
alone do not prove that resources were released.

Before terminal, an explicit repeated cancel can redeliver the idempotent native
stop request to retry unfinished cleanup. The driver does not repeatedly request
stop merely because a resource remains open. Asset-editor sessions keep their
native resource and report `cleanup_pending` until actual closure is observed;
recovering and closing the same operation can retry after the cause is resolved.

`cleanup_complete` records confirmed native cleanup separately from the business
outcome. Progress failures remain in `progress_diagnostic`; unknown outcomes
without cleanup proof cannot be removed by `operation.release`. `receipt()` is
terminal-only. SDK `finish/close` saves the receipt before removing the retained
server record. Capture waits for Game Thread cancellation before confirming
Release. Shutdown requests cancellation and drains ordinary operations while
the Editor can still process Game Thread cleanup. Host-exit operations instead
complete through the dedicated external witness.

Finite PCG generation sessions finish through native task observation and the
same Join/Release path. Cancellation targets only the session-owned generation;
it does not delete successful output or promise rollback. The final signal
distinguishes this generation's completion from pre-existing component output.

## Legacy exit

`Composition/LongRunningSession` remains the internal owner behind exact frozen
contracts. The SDK no longer relies on manual post-response checkpointing or
separate PIE/import/preview polling loops. Historical checkpoint and preview
receipt formats retain read compatibility only. T44's old compiled registry/HTTP
handler remains retired; no legacy prefix, alias or fallback activates this owner.
