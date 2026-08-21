# T43 Reference operation authority

`reference.asset_apply` has one public authority,
`reference-operation-owner`. Its default SDK entry is
client.reference.asset_apply(...), and its exact CLI entry is `uai-cli ... exec
reference.asset_apply`. Callers obtain fields, contract hash, failure codes,
and tutorial links from `command_search` and `command_detail`; no task-specific
generated projection remains active.

The owner sends one exact `object_path`, `intent`, and optional
`proposed_text` request through the common operation lifecycle and existing
native ReferenceCrud mechanics. It does not reimplement path normalization,
mutation, persistence, readback, or cleanup in the SDK.

Terminal success is corroborated by an independent persisted public read.
That read uses the single public `reference.asset_read` Query route, whose
server-owned `asset.properties` mechanics selects the exact persisted
`BlueprintDisplayName` written by this operation; an asset-summary existence
check is not a sufficient oracle.
Prewrite intent or target rejection leaves no residue;
`operation_state_unknown` requires reconciliation. Cleanup is bounded to the
owned reference fixture and verified by an independent absence read.

T44 physically isolated the former module-owned Query-service composition and
its `Composition/ReferenceCommand` apply handler. Neither source is compiled or
discoverable as an active test, so the current T43 handler is the only executable
public owner.
