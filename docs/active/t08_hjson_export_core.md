# T08 Verified HJSON Export Core

## Scope

T08 establishes the common artifact-publication lifecycle used after a domain captures real UE state into the unified HJSON model. It is a module contract only: it does not claim that any asset domain or public export command has completed migration.

The mandatory direction is:

`freeze target revision -> capture complete workspace and semantic signature -> sibling staging -> canonical HJSON reparse -> semantic rebuild -> semantic equality -> target revision recheck -> atomic publish`

The capture source is the owner of the real target read and of the parser/resolver used to rebuild the staged workspace. A complete semantic signature covers inventory, reference closure, content digests, contract semantics, and business semantics. Publication cannot begin unless every component rebuilt from the staged files equals the capture signature.

## Verified export boundary

`TryExportVerifiedWorkspace` accepts one `IVerifiedExportWorkspaceSource`. The source freezes an exact `FUeAgentStateRevision`, captures a stable `FExportWorkspacePlan` bound to that revision, rebuilds semantics from the physical staging path, and independently reads the current target revision immediately before publication. A captured plan with a different revision is rejected before staging.

The verified path delegates physical ownership, sibling staging and backup paths, journal handling, hash and inventory verification, rollback, and final replacement to the existing artifact publisher. Staging is adjacent to the final path, so the final move remains on the same volume. A semantic failure or version conflict removes only transaction-owned staging state and leaves the previous final untouched. A commit failure restores the previous owned final.

Legacy `TryPublishExportPlan` remains an internal physical publisher for existing callers. It does not by itself make a verified-export or domain-completeness claim. New public domain export routes must enter through the verified path and provide a real capture/rebuild source.

## Token and resource semantics

The verified export request has no token-budget parameter. `FExportWorkspacePlan::ObservedTokens` is observation telemetry only; changing it cannot change files, hashes, semantic signatures, or publication eligibility. Token limits may page a read projection, but they cannot reject or truncate complete export.

Real file, byte, node, item, reference-edge, work, and memory budgets remain enforceable through `FUeAgentResourceTracker`. Their failures retain typed resource errors and never publish a partial workspace.

## Receipt and typed failures

A successful verified receipt records that staged semantics were verified and the target version was rechecked, and binds all five semantic-signature fields alongside snapshot revision, manifest inventory, file hashes, and plan fingerprint.

Stable verified-export failures include invalid request or capture, capture/frozen revision mismatch, semantic rebuild failure, invalid semantic signature, `artifact_semantic_mismatch`, revision recheck failure, and `artifact_target_version_conflict`. Physical publisher failures keep their existing typed staging, ownership, hash, resource, publish, rollback, verification, and cleanup semantics.
