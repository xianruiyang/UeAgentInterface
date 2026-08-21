# Retired strict_v2 final-audit bundle

This directory preserves the former T90/T99 retirement manifest, final-audit
document, Automation oracle, and the old module-owned Query-service composition
as historical evidence only.

T44 removed them from active paths because the T99 oracle hard-coded a stale
342-command count and an obsolete task-controller text assertion, while
`FUeAgentQueryServiceComposition` and its `ReferenceCommand` handler formed a
compiled alternate composition seam no longer used by module startup. The
current authorities are the immutable command manifest, frozen
`DomainCapability` composition, `docs/active/capability-catalog/` v2 historical
dispositions, and the root `plan/` workflow and task table.

Nothing under this directory is built, discovered as an active test, published
as current documentation, or used to decide task state or completion. Restore a
file to an active path only after a current owner, consumer, contract, and
verification oracle have been explicitly re-established; do not restore the
T90/T99 task identities or the old Query-service composition as compatibility
entries.
