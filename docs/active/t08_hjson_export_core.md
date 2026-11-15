# T08 Verified HJSON Export Core

## Scope

T08 describes the artifact-publication lifecycle after a domain captures real UE state into an export plan. Domain-specific capture remains responsible for values and references; the publisher owns physical publication.

The mandatory direction is:

`freeze target revision -> capture export plan -> sibling staging -> verify actual staged bytes and ownership -> target revision recheck -> atomic publish`

The capture source owns the real target read and correct construction of the output. Ordinary publication does not parse generated HJSON again or rebuild a second semantic model. The former five signature fields were derived from the same file/model digest and supplied no independent UE evidence; their production and comparison have been removed. Import still parses author input through its normal source owner.

The composition owner also provides its actual immutable model, layout,
inventory order, namespace and row spans to the next directory-source read.
The source still observes current metadata and author-file bytes. It builds
typed inventory rows only for changed documents or documents with external
content; missing optional row material uses the existing reader for that shard.
This handoff does not prove current UE values or authorize publication. Legacy
inputs retain their reader, and source fingerprints keep their existing ordered
material and algorithm. Exact diagnostic nodes are read from the observed
inventory only when needed, rather than retained for every unchanged unit.

An existing-directory full export can use the original publisher ownership table
when every managed input still matches its actually published bytes. The source
then reads header, namespace, naming and inventory metadata through the same
parser, without rebuilding old captured values, a complete old source model or
a persistent syntax archive that no consumer needs. Current UE capture remains
the output authority. A changed author file or unsupported legacy proof uses the
original semantic reader, including its unapplied-edit and source diagnostics.
The refresh checkpoint still fences observed source bytes and consumer progress.
After publication the next source read receives the model and layout belonging
to the actual produced plan, including any definition rebinding. Optional
metadata admission never authorizes a native edit.

Machine inventory and captured-envelope documents keep the existing schema.
Fresh and explicit full exports group nested item documents by their existing
top-level item owner, with independent groups for global documents. Their stable
file names are hashes of those existing scopes. An ordinary edit of an older
workspace keeps its legacy buckets; explicit full can reorganize them once.
The manifest declares inventory paths and each row declares its envelope path,
so readers continue to accept both organizations. Source views, row spans and
retained membership use the actual producing generation's organization. A move
does not reuse a thin row carrying the old envelope reference. Publication
still verifies current bytes and ownership, and system reorganization is not
an author edit or a change to the semantic fingerprint.

Hidden captured baselines use the same producer's actual unit span only beside
the observed full-file hash. The original HJSON parser then constructs just the
needed envelope row with its original source unit, pointer, absolute bytes and
line/column; captured types, values, definitions and metadata conflicts still
use the same semantic reader. A fragment is never cached as the whole document.
Missing optional spans or a different producer/hash use the existing full reader.

A scoped semantic import fences the files actually read by its source proof,
including the classified workspace header and any referenced external content.
It does not reject an edit because an unread guide changed. The same proof
is checked before native application and publication. The formatter marks
which generated files can feed the next source read; this only limits optional
observation retention. Publication still checks every file it stages or replaces.
Prior-publication observation references are released when source consumption
starts; live input proofs keep their own references and later publication
reacquires or reads through the existing path. Legacy unscoped readers keep
their namespace snapshots. No cache admission becomes a business condition.

The committed-difference owner also transfers the actual merged additions,
replacements and removals to projection beside their exact immutable
baseline. This includes dependency projections and observed side effects,
rather than the author edit list. The unchanged-membership layout reuse path
also consumes this same delta beside the exact accepted binding/layout owner;
it does not reallocate every unchanged alias. Current value/layout and global
definition checks remain. Other owners and legacy input retain the comparison path. Optional composition-key tables stay with the old
owner until its last comparison/schema consumer finishes, then move to
the next plan with changed keys applied. They are not source, publication
or native authority. Compiler identity lookup shares existing declaration
strings; a native-target index is built only for an actual identity query.

The layout inventory reads identities and successful public-row SHA values from
the immutable semantic units, rather than copying a complete identity/hash array.
The row SHA uses the original row formatter and UTF-8 boundary; it is distinct
from edit hashes, the model fingerprint and placement-dependent composition keys.
Readable refs belong to the same immutable document rows as logical/physical
placement. All three lookup views share the compiler's identity index and a
location array; they do not keep a second identity map or a complete ref map.
Document iteration preserves physical-file/row order. Standalone reference
projection keeps its existing map API at that boundary. Reverse source lookup
borrows the document-owned strings, never temporary iterator pairs.

Committed member updates merge sorted additions with the prior ordered members.
Compiler owner-cycle validation follows complete current paths from new or
changed owner edges beside an already valid graph; cold/unknown graphs still
start at every unit, and missing-reference and target-kind checks remain current.
Neither optional reuse nor this traversal selection creates a new edit rejection.

Level native comparison follows the actual current request identities, not the
old snapshot count. Successful observations are selected into the current scope;
new identities and changed class/field/schema owners are read through the same
native capture. The existing Snapshot owner merges only those captured entries
and bindings. Unselected observations retire inside the request, under the old
native-class roots, and their time remains reported. The final reuse owner builds
class/schema metadata and semantic facts for the resulting current scope and
keeps the existing GC/unload rules. No full older value generation is retained.

Accepted input identity uses the resolver's exact observed source digests and
original metadata/row consumption order. Restoring values through the cold parser
and compiler does not change that identity compared with a generated source view.
The former full-reader formula is a compatibility value only for that same scoped
resolution. Committed refresh still verifies the current source namespace/bytes;
identity matching does not authorize replaying a native action.

Machine inventory and captured-baseline documents use the original full-span
parser and source diagnostics, without archiving a complete syntax tree after
compact rows and typed values have been consumed. Editable document syntax reuse
and the real source/publication proofs retain their existing owners and budgets.

## Verified export boundary

`TryExportVerifiedWorkspace` accepts one `IVerifiedExportWorkspaceSource`. The source freezes an exact `FUeAgentStateRevision`, captures a stable `FExportWorkspacePlan` bound to that revision, and independently reads the current target revision immediately before publication. A captured plan with a different revision is rejected before staging.

The verified path delegates physical ownership, sibling staging and backup paths, journal handling, actual byte/hash and inventory verification, rollback, and final replacement to the existing artifact publisher. Staging is adjacent to the final path, so the final move remains on the same volume. A staging failure or version conflict removes only transaction-owned staging state and leaves the previous final untouched. A commit failure restores the previous owned final.

`TryPublishExportPlan` remains the physical publisher used by existing domain callers. It makes no independent domain-completeness claim. A route using the verified wrapper additionally supplies real capture and target-version recheck; an unused wrapper is not a reason to add a second capture or semantic rebuild. The owned workspace is replaced atomically.

After final verification, the receipt reports publication even when backup or
journal cleanup fails. Its `warnings` array identifies residual cleanup work;
these warnings do not turn an already-published workspace into a failed export.
The public export response keeps `status: "succeeded"` and projects these codes
as top-level `warnings`; machine receipt identities remain private.
The publisher preserves its journal when backup cleanup is incomplete.

Level item queries consume the existing actor/component topology and state,
including the same public geometry, names and identities. They do not first
materialize unrequested ordinary property values. Property selection keeps its
existing topology/value split; other domains and partition loading keep their
original capture owners. Capture revisions describe the actual observed scope.

Generation publication reads actual current hashes and the guarded ownership
record to decide its write set. A same-request accepted raw-byte observation can
supply an untouched candidate only while its original data lease is current and
the publisher has the real source checkpoint. Canonical UTF-8 shares its existing
text digest; other encodings keep their actual separate byte digest. Both logical
path/bytes checkpoints still run. Other untouched files use a short strong read
guard; a lease with no later consumer is not retained or produced for them.
Changing/deleting, staged and ownership files keep their original kernel proofs. Only files that will be replaced or deleted keep
long-lived commit guards; equal, untouched files are not staged or copied into
the recovery journal. Their real bytes remain in place. The original source and
consumer-watermark checkpoint runs before staging and immediately before DELETE
access upgrades; each upgrade still rechecks the same old hash. Namespace parent
protection, staged readback, changed-file recovery and the original guard/proof
budgets remain. A directory's total file count is not a substitute for this write
set. Read-only status matters when a file must actually be replaced or deleted.
A later author change to an untouched file is preserved against the published
baseline and is not silently consumed as generated output.

## Read-only authoring guides

Follow the current `examples/index.hjson` and per-file guide. Shared type recipes
use their generated content identity, so a different encounter order does not
renumber an unchanged recipe and rewrite unrelated guides. These paths are
reading aids, not business identities or import defaults. Refresh replaces old
managed `#` recipe paths and updates the guides together; existing author values,
definition/public references and readable aliases remain valid. The publisher's
protection for changed author bytes still applies to managed guide replacement.

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
Physical file hashes and actual staged byte checks remain publisher-owned.
Author input assembly is performed when that input is imported, rather than as
a second model built from generated output on every publication.

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

A successful verified receipt records the actual target-version recheck alongside snapshot revision, manifest inventory, file hashes, and plan fingerprint. Compatibility getters for semantic verification report false/empty; they do not claim a verification that no longer runs.

Verified-export failures include invalid request or capture, capture/frozen revision mismatch, revision recheck failure, and `artifact_target_version_conflict`. The retired rebuild/signature comparisons no longer reject publication. Physical publisher failures retain typed staging, ownership, hash, resource, publish, rollback, verification, and cleanup semantics.


For Level property-only public reads, the same read hook resolves public
Actor/Component names from the original hierarchy projection before values are
captured. The original selector resolves owners and containers; the Level native
snapshot then observes only their requested fields. Type/default/presence and
source projection still use the original codec and read owner. The selected
observation revision is not a native identity or a claim about unrequested
properties. Partitioned worlds retain their original complete-loading path.

Example publication proofs borrow the exact guide file from Composition's
existing file-plan owner by path and content hash. They do not retain and charge
another complete guide byte plan. An unchanged source document can use the
formatter's already computed output hash when its transient canonical view has
expired. Actual recipe lookups, including missing results, and overlapping
rule/collection/content-ref contributions keep their original checks and order.
The same bounded proof admission falls back to generation on a miss.
