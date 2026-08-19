# T10 Operation Lifecycle Core

## Scope

`FOperationLifecycleService` is the common server-side owner for operation sequencing. T10 proves the module contract only; its scripted ports do not claim that a real UE domain write is production-ready.

The service owns this order: validated plan, exact impact authorization, pre-execute version fence, domain mutation port, settle, pre-persist version fence, persistence, independent readback, and terminal receipt. Domain ports implement only real domain mutation and compensation mechanics. They do not authorize impact, persist, verify themselves, or decide the final result.

## Plan and authorization

An operation plan contains one content-addressed plan fingerprint, the complete impact scope, an expected revision for every impacted identity, and explicit settle signals. Authorization is valid only for the exact plan fingerprint and exact impact set. A new target or changed plan invalidates the authorization; the service never expands it during execution.

Version fences run before the first mutation and again after settle before persistence. A stale pre-execute revision is a no-change rejection. A stale pre-persist revision enters compensation because mutation has already occurred.

## Terminals

- `succeeded`: settle, persistence, and independent readback all completed and the readback matched.
- `rejected`: no mutation began; retry is explicit and safe.
- `rolled_back`: mutation began, a later stage failed, and compensation was independently confirmed; retry is explicit and safe.
- `state_unknown`: mutation began but compensation was unavailable, failed, or could not be confirmed. Automatic replay is forbidden.

Every receipt records the phase history, whether mutation began, retry safety, the error code, and the persistence receipt when success is verified. A domain response, an in-memory state, or a successful save alone cannot create the success terminal.

## Historical control retirement

The removed public IDs `begin_transaction` and `end_transaction` are represented by the internal architecture contract `uai.operation.atomic_change_set/1`; they are not aliases and no cross-request transaction handle remains. The removed public IDs `save_asset` and `save_current_level` are represented by `uai.operation.persistence_and_independent_readback/1`; persistence is a mandatory operation stage instead of an optional follow-up command.

`external.texture.import` is a current public representative that runs through both contracts. This module-level contract does not claim that every domain write is complete. In particular, the Level/World owner must separately prove directory-workspace mutation, save, reload, and independent readback before its task can close.
