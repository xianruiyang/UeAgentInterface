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

and never publish a partial workspace.

After final verification, the receipt reports publication even when backup or
journal cleanup fails. Its `warnings` array identifies residual cleanup work;
these warnings do not turn an already-published workspace into a failed export.
The public export response keeps `status: "succeeded"` and projects these codes
as top-level `warnings`; machine receipt identities remain private.
The publisher preserves its journal when backup cleanup is incomplete.

## Content-type organization

The composition writer organizes the already-projected editable value through
`UeAgentAuthoringContentLayout`. Domains declare content boundaries; samples,
curves, events, records and MovieScene timelines use the same file publisher.
Graphs retain their existing shared projection and independent property units.
This is physical organization, not a new domain codec or public command.

Ownership determines location; content type determines representation and edit
granularity. Generated property surfaces are not asset subobjects and do not add
an `items/target_settings-N` directory. Content paths do not embed public-ref
handles. Sequence bindings contain track files, and UMG animations contain their
bindings and tracks. Niagara stacks retain the emitter relation carried by the
capture, including when two emitters have the same script usage.
Ordinary state, properties, ordering and relation documents follow the same owner
directory as its content fragments. Existing graph-specific placements are retained.
Graph node directories reuse the graph-visible node aliases from the shared graph
projection (for example, `graphs/<graph>/items/Posts/properties.hjson`). Both the
node key and physical navigation use the same node membership and semantic-name
selection. Generic semantic roles such as `graph_collection` do not name these
node directories. Logical unit identities and old workspace imports are unchanged.
Node aliases prefer captured business semantics and owned native naming properties
(including `ParameterName`, `InputName`, `OutputName` and named-node `Name`) over
generic node types. A unique name has no numeric suffix; suffixes disambiguate
collisions, never creation order, and do not consume another node's business name.
Graph keys, connections, property paths and Material graph queries reuse this same
projection. The node's `name` still describes its type/candidate where the domain
uses that contract; the alias is not a replacement native identity or editable name
property. After refresh, use the current aliases and `properties_ref` paths.
Business graph names such as `UserConstructionScript` remain valid names: the
readable-name filter does not interpret ordinary 22-character names as base64
Short GUIDs. Explicit hexadecimal UUID forms remain machine-only.
Embedded hexadecimal runs in business names are preserved. Complete digests are
filtered only when a fallback comes from an identity/source field; Blueprint map
keys reuse this same identity check. Empty or whitespace-only preferred names
allow the remaining business-name sources to be considered. Semantic path names
use the same letter/digit classification as graph aliases and the common Windows
filename sanitizer, including reserved device-name handling. Workspace root-path
constraints are unaffected.

Typical paths relative to a target are:

```text
properties.hjson
bindings/<binding>/tracks/<track>.hjson
animations/<animation>/bindings/<binding>/tracks/<track>.hjson
emitters/<emitter>/stacks/<usage>/modules/<module>.hjson
float_curves/<curve>.hjson
bone_tracks/<bone>.hjson
```

Collections and composite owners use `index.hjson` at their own directory;
simple members use their business key as the leaf filename. Names are sanitized
as individual path segments, with sibling collision handling; business keys in
the HJSON remain unchanged. Sections/channels/keys remain inside their track.

A main document uses `content_ref` with a workspace-relative path; the referenced
ordinary HJSON contains the editable `value`. The inventory records each unit's
`content_files`. A curve retains its keys and tangents, a sampled bone retains its
whole track, and a pose matrix retains its aligned columns. Named collections
keep an editable index, including empty collections. Values are not duplicated
between the main document and its content files.

The original unit and edit hashes describe the assembled semantic/public value.
Physical file hashes remain publisher-owned. Staged semantic rebuild uses the
same content assembly as import before comparing against the captured model.

Semantic directory import also reuses the target's exporter after successful
apply/readback. The resulting canonical export is atomically published over the
input workspace, retaining files outside its managed authoring set. Thus creation
may use inline values or new `content_ref` files while the completed workspace
returns to this standard layout, without duplicate values or consumed temporary
fragments. A `workspace_refresh.status=refreshed` result identifies the new usable
baseline; callers need not request another export merely to continue editing.

The current content declarations are:

| Content boundary | Consumers | File unit |
| --- | --- | --- |
| Graph | Existing shared graph projection; Control Rig graph collection | One complete graph; nodes and links stay together |
| Record | DataTable rows, Niagara modules, physics bodies/constraints, IK goals/solvers/retarget operations/mappings, Blackboard keys | One named record plus its collection index |
| Hierarchy | Rig elements, IK chains, StateTree and SmartObject items | One named item; references remain native collection keys |
| Curve | AnimSequence float/transform curves, CurveTable rows, Curve assets | Whole curve and related keys/tangents |
| Samples | AnimSequence bone tracks, retarget pose groups, PoseAsset matrices, PCG data sets | One coherent track/group/matrix; aligned values stay together |
| Timeline | UMG animations and LevelSequence bindings/tracks; Montage slots/sections | Animation/binding/track, or ordered Montage collection; keys stay with tracks |
| Events/settings | Animation notifies/markers, Skeleton collections, StaticMesh collision/build settings, MPC parameter groups | Coherent collection/settings group |

Physical record files do not create independent semantic units. Order arrays,
references, row schemas and native apply remain owned by the original collection.
Short declarations, string/localization tables, ordinary reflected properties and
already-independent object/graph documents retain their existing organization.
The organizer does not recursively split arbitrary business objects.

## Receipt and typed failures

A successful verified receipt records that staged semantics were verified and the target version was rechecked, and binds all five semantic-signature fields alongside snapshot revision, manifest inventory, file hashes, and plan fingerprint.

Stable verified-export failures include invalid request or capture, capture/frozen revision mismatch, semantic rebuild failure, invalid semantic signature, `artifact_semantic_mismatch`, revision recheck failure, and `artifact_target_version_conflict`. Physical publisher failures keep their existing typed staging, ownership, hash, resource, publish, rollback, verification, and cleanup semantics.
