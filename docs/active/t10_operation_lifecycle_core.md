# T10 Operation Lifecycle Core

## Scope

`FOperationLifecycleService` is the common server-side owner for operation sequencing. T10 proves the module contract only; its scripted ports do not claim that a real UE domain write is production-ready.

The service owns this order: validated plan, exact impact authorization, pre-execute version fence, domain mutation port, settle, pre-persist version fence, persistence, independent readback, and terminal receipt. Domain ports implement only real domain mutation and compensation mechanics. They do not authorize impact, persist, verify themselves, or decide the final result.

## Plan and authorization

An operation plan contains one content-addressed plan fingerprint, the complete impact scope, an expected revision for every impacted identity, and explicit settle signals. Authorization is valid only for the exact plan fingerprint and exact impact set. A new target or changed plan invalidates the authorization; the service never expands it during execution.

Version fences run before the first mutation and again after settle before persistence. A stale pre-execute revision is a no-change rejection. A stale pre-persist revision enters compensation because mutation has already occurred.

## Terminals

- `succeeded`: required native execution, settle, persistence and reload completed. A semantic readback difference is a warning, not a failed execution.
- `rejected`: no mutation began; retry is explicit and safe.
- `rolled_back`: mutation began, a later stage failed, and compensation was independently confirmed; retry is explicit and safe.
- `state_unknown`: mutation may have begun, or recovery could not be confirmed. Automatic replay is forbidden.

Every receipt records the phase history, whether mutation began, retry safety, errors, warnings and the persistence receipt. Input validation and actual execution/IO failures retain their error semantics. Semantic observations do not authorize rollback or rewriting native state. Durable journals use `committed` when execution completed without an equivalence proof and `verified` when that proof is available; both are completed journals, and neither is an invitation to replay the operation.

The typed execution path also reports `failed` when the native adapter explicitly
confirms that no target mutation began and the prepared durable session is closed.
An empty/default adapter result does not supply that confirmation. Failure while
closing durability remains unknown and unsafe to replay; the original operation
error is retained.

## Persistent HJSON authoring

The authoring implementation uses `OperationCoordinator`, `WorkspaceTransaction`,
and `DurableTransaction` for prepared native writes. Both public
`DomainHjsonAuthoringKit` workspace coordinators always supply `FinalReadback`:
transaction-owned persistence, reload and final read-only observation finish before
commit. Missing required persistence or reload remains an execution failure. The
domain adapter supplies native state release, rebinding and read requests. There
is no coordinator switch or post-commit save/reload finalizer. Separate typed
helpers retain their own lifecycle contracts; this rule does not add a second
backup or commit owner.

Before mutation, the durable owner captures a package checkpoint of the current
memory state separately from the original disk backup. This distinction preserves
unsaved edits. After native state has been released, recovery reloads the memory
checkpoint, restores the original disk artifacts and dirty flags, and compares
the restored semantic state with the captured pre-state. Before/restore read
requests address the real original objects; post-apply requests may address new
objects. Pointer rebinding alone cannot restore a deleted nested object graph.

Unconfirmed recovery retains the checkpoint, disk backup, and journal and reports
an unknown state. A later disk-only recovery does not discard unconfirmed memory
evidence. A genuine no-op performs only final readback, without creating a journal
or unloading the package. Semantic readback differences are warnings even for a
no-op and never start compensation. Recovery verification after an actual execution
failure remains strict: an unconfirmed restoration must not be reported as restored.

Historical real-consumer evidence covers MetaSound and Material update paths,
including recovery with distinct dirty-memory and disk pre-states. Historical
semantic-mismatch rejection is no longer the production success rule. The shared
component retains no-op and unconfirmed-recovery evidence.
Other providers require explicit adoption and their own native
lifecycle evidence; these results do not certify all HJSON consumers.

## Historical control retirement

The removed public IDs `begin_transaction` and `end_transaction` are represented by the internal architecture contract `uai.operation.atomic_change_set/1`; they are not aliases and no cross-request transaction handle remains. The removed public IDs `save_asset` and `save_current_level` are represented by `uai.operation.persistence_and_independent_readback/1`; persistence is a mandatory operation stage instead of an optional follow-up command.

`external.texture.import` is a current public representative that runs through both contracts. This module-level contract does not claim that every domain write is complete. In particular, the Level/World owner must separately prove directory-workspace mutation, save, reload, and independent readback before its task can close.
