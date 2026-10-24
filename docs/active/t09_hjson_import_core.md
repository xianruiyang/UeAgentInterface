# T09 Unified HJSON Import Planning Core

## Scope

T09 establishes the side-effect-free planning and provider-resolution boundary between the unified HJSON semantic model and later domain execution. It proves common parsing output, explicit change semantics, prewrite validation, initialization-graph planning, unique domain-provider registration and resolution, resource failure, and cleanup behavior. It does not claim a public domain import or any real UE object creation; T13 and the domain tasks consume this plan through their registered native creation providers.

The only import-planning direction is:

`directory | single_file | inline -> TryLoadHjsonImportSource -> unified semantic model -> explicit change set -> preflight -> initialization graph -> exact provider -> exact impact authorization -> immutable plan`

`TryLoadHjsonImportSource` is the common side-effect-free source boundary. A semantic `directory` fixes one normalized root and resolves its manifest units and recursively referenced content, including newly authored files. Unreferenced user files are not semantic inputs. Referenced paths, parsing and semantic types retain their existing validation. Legacy content-graph bundles retain their declared-file/digest contract. `single_file` and `inline` validate the embedded manifest and units through the same resolver. Multi-file content exists only inside a directory workspace; it is not a fourth public source variant or a separate business route.

Once the three public carriers resolve to the same `FUnifiedHjsonSemanticModel`, they produce the same planning semantics. Full-state, minimal-create, and local-edit intent remains on that common model. Omitted fields, items, relations, order, and layout do not synthesize deletion; create, delete, replace, clear, reorder, connect, disconnect, set, null, restore-default, and remove are explicit change kinds.

## Prewrite contract

The common resolver assembles `content_ref` before `ParseSemanticUnit`, including
units with no previously exported fragments. A reference is a single-field object
whose string path is workspace-root-relative; its target supplies `value`.
The export inventory's `content_files` records ownership, not admission permission.
New references use the same parser, path checks and cycle detection as old ones;
missing files or invalid values report actual errors. Assembly preserves scalar
lexemes and feeds existing type resolution, semantic diff, keyed desired-state
handling and native providers. No domain parser loads fragments, and callers do
not edit hidden inventories or hashes. Managed paths include old inventory files
and newly consumed references; unrelated files remain outside the asset model.

Property type assertion failures retain their property and original source during preparation,
before provider application. A public type leaf is an exact source; a type stored only in the
hidden envelope uses the associated authored property as `related_input`, without claiming that
the value contains the invalid type declaration. Document diagnostic lookup consumes the
producer's semantic/internal pointer as well as physical pointers, preserving existing concrete
file locations and resolving transformed documents through their original source bindings.
Initial and single-file imports retain the decoded AST through a shared diagnostic source scope.
The scope binds the original JSON objects without inserting metadata, resolves typed failures and
nested causes to parser coordinates, and preserves already precise sources. Generated World/HLOD
units carry explicit source resolvers back to the original document. A missing field or native
failure without an authored leaf uses an existing ancestor as `related_input`; internal generated
line numbers are not presented as original HJSON coordinates.
The external-input factory, skin-weight and localization routes are also `import_workspace`
consumers. They carry prepared JSON values together with the original AST or semantic workspace
through parameter validation and synchronous importer delegation; no intermediate HJSON is
serialized and reparsed. Delegated parameter pointers map back to source/options/target-policy
fields. The importer returns the lifecycle's final diagnostic, including rollback causes,
before original-file locations are attached. Existing precise native-file locations are retained.

## Successful directory import refresh

After native apply and final readback succeed, the public source-custody coordinator
calls the target's existing exporter into a temporary sibling directory. The shared
artifact publisher then publishes those canonical files back to the source root,
using its existing staging, backup, verification and rollback lifecycle. Files
outside the old/current managed set are retained byte-for-byte, including binary
files and unrelated HJSON. A canonical path collision with a retained user file
is reported rather than overwriting that file.

This returns newly created inline nodes/properties and custom fragments to the
normal type-based layout and updates the import baseline. The response includes
`workspace_refresh.status=refreshed`; the caller can edit that directory again.
`workspace_refresh.warnings` reports cleanup problems after successful publication;
these warnings do not invalidate the refreshed workspace or request a repeat import.
If refresh fails after asset commit, the command fails with
`content_graph_import_applied_workspace_refresh_failed`; its detail explicitly
reports `asset_applied=true`, `retry_import=false` and recovery instructions.
This does not relabel an applied asset operation as rolled back. Failed asset
apply never refreshes the source. Inline/single-file creates and legacy bundles
do not acquire this semantic-directory refresh behavior.

`TryPlanHjsonImportInitialization` validates the source baseline, exact target
revision, every explicit change, the server-resolved candidate binding,
initialization node and edge, impact scope, domain support, and authorization
before publishing a plan. Candidate names are passed to the preflight owner
unchanged; an implicit alias or remap fails with
`hjson_import_candidate_identity_changed`. Any manifest fingerprint remains an
internal planning detail and is not a candidate selector.

Directory workspace imports reconcile the captured baseline, current native
capture, and explicitly edited units before building the change set. Unedited
state (including preview lifecycle changes) comes from the current capture.
Change planning, baseline reconciliation, document field projection and ordinary
struct-member pruning use the captured formal type/definition and the shared
property codec's storage equivalence. This does not relax identity/dependency
hashes, invent types for undeclared values, or remove explicit values from direct
patches and complete collection replacements.
An unchanged edited unit and its unchanged explicit dependencies can be replayed across
unrelated changes. Declared plain `object` documents merge their named members;
reflected structs merge declared members while type, schema and identity remain unchanged.
Members without a descriptor remain atomic: their JSON shape does not authorize deeper
merging or invent array element/reference identity. Object member descriptors also allow
declared nested records to merge using the same rule.
Compact authored values first use the existing import overlay against their captured
values, so omitted reflected members retain their values and explicit null retains
its normal meaning. Native top-level fields and document `value` wrappers keep
their existing representations. JSON object shape alone never authorizes a merge.
Concurrent scalar/reference/container changes, native structure changes, identity/type changes,
and changed declared dependencies are conflicts, not guessed conversions.
Dependency traversal follows explicit dependency and field-set contracts. Owned child/relation
inventories, layout and ordering are retained from the current capture unless actually edited;
an unedited child's reconstruction does not invalidate its parent's unrelated value edit.

The existing machine envelope retains captured values for all units, bound to
the inventory content hash. No new editable files or client-side hash maintenance
are required. Older hash-only workspaces can replay an unchanged edited unit;
they cannot authorize a merge requiring missing original values. Inline sources
without explicit edit/baseline evidence retain their revision requirement.
Prepared changes always bind the current snapshot; save/reload/rollback behavior
is unchanged. A rebase only changes the in-memory prepared model, not source files.
Final value retention observations use the immutable original edit before domain projection
and a fresh native capture. Native owners declare identity, ordering and numeric
normalization through shared comparison/projection; omitted defaults in new records
are retained, while explicit fields remain observable. These comparisons diagnose
consistency; they do not reject valid input, roll back completed native operations,
or replay old values over native updates. Input validity and actual execution/IO
failures remain errors. Graph layout coordinates use a
shared exact decimal representation for export/resolve stability; native integer
truncation, integer rounding and vector movement rules remain distinct.
Native normalization also retains the frozen decoded argument sent to the native
owner on the isolated proposal. The original edit is first checked against that
argument using the shared typed semantics; only then may the isolated native result
normalize its owned value path for independent final readback. The result is
captured after the isolated proposal finishes native updates, not from an early
setter or the actual target. This includes array ordering, generated identities
and native curve clocks without a global tolerance or fixed sampling rate.
Value normalization evidence is keyed by both native target identity and semantic
entry: metadata, seed and editable document views of one object do not borrow
each other's value paths.
ObjectApply's decoded-property proof separately freezes the input passed to the
decoder. Its transmission check reuses the formal property-edit projection of
the immutable prepared change, including ordinary structs' omission of unchanged
members. It compares that projected input without borrowing the final native
capture's type or value definition: compact owned-property objects are not native
descriptor arrays, and sparse struct edits are not full native values. The captured
typed property intent then verifies the fresh native object or struct independently.
This does not replace the typed normalization proof above or skip native verification.
Declared `name` values follow native FName semantics; ordinary strings stay exact.
Keyed collection contracts distinguish independent mutations from complete-set
reconstruction: the latter retain unchanged captured rows as context, without
marking those rows as edits or losing omitted-item removal instructions.
Aggregate original intent compares baseline, request and native readback at the
same composed semantic layer. Declared rank groups and derived identity fields
retain original aliases across native reorder or binding-path changes; ordinary
reflected properties are not reinterpreted merely because a field name matches.
Ordinary arrays remain the authored sequence, including omitted record members;
equal length does not establish item identity. The existing native owner supplies
defaults or retains values in its real context. Only declared owned/structured
`instance` bindings restore captured items before that boundary. Fresh aggregate
captures align duplicate-label aliases with the request by native identity before
interpreting the original operation records.
If composition omits a name or label equal to its map alias, original-intent
projection restores that value before identity alignment. Explicit duplicate
labels remain explicit; frozen constructor references remain native identities.
After that binding, the collection's existing public field policy selects the
business values to verify. Semantic unit IDs and other captured binding metadata
are not editable intent; native derived-identity changes may regenerate them.
Explicit public field changes and requested membership still participate in readback
observations; differences are returned as warnings.
Reflected property deltas use the frozen baseline's same authoring projection as
export and property restore. Hidden non-editable struct members are native
preservation state, not requested removals. Final expected values overlay those
public edits onto the fresh captured value. Codec normalization uses the same
fresh read request's struct-member filter for both decode and encode, including
explicitly authorized members. A public capture may exclude internal members even
when its detached native storage contains them; verification must not widen that
scope. Normalization failures retain their underlying cause and source location.
The authoring-state/canonical-snapshot bridge retains the captured type, member
definition and candidate metadata in the existing authoring descriptor. Native
comparison-only definitions remain outside that editable descriptor.
Ranks are declared ordering values, not storage indices: finite integral ranks
retain the native writer's accepted range and are compacted only in its declared
groups. Constructor defaults and selected types are frozen before publication by
the native creation owner in the existing import normalization scope. StateTree,
SmartObject and Blackboard consume the same keyed argument projection; selector
metadata is not persistent business state. Explicit created-row fields still
participate in original-intent verification. Created inline properties decoded
and verified on a native template may record canonical typed intent for final
fresh capture; transient template storage is never retained across reload.
Inline category projection retains the original parser node and annotates apply
descriptors with the existing authoring-source location. Native descriptor refresh
reattaches those locations by property path; error locations never come from a
synthetic document named after the created item.

Final intent evidence follows the provider's real capture contract. Authoring-state
providers regenerate native read requests after mutation. Canonical content-capture
providers retain their original property/relationship requests through their native
transaction and rebind them after durable reload; a private transaction snapshot
with different semantic identities is not a substitute for public workspace capture.
Preview-only changes have no asset save lifecycle. Structural creation, replacement,
deletion and links are verified by their native mutation owner, separately from
stable value entries. Absence of the authoring-state callback alone does not prove
absence of intent verification; its actual owner, input and lifecycle must be checked.

Conflicts retain `hjson_import_stale_base_revision` with a structured
`workspace_rebase` diagnostic: affected source/semantic path, reason and known
old/current/proposed scalar values. Composite differences carry concise summaries
rather than entire graph dumps. All identified independent conflicts are returned
before mutation. Preserve the edited directory, export to a new name and resolve
the reported conflicts; never replace the workspace baseline by hand.

The planner owns no UE mutation, save, reload, rollback, or client-side name mapping. A failure resets the output plan and never calls authorization when an earlier resource, version, candidate, support, or graph check has failed. The authorization decision is bound to the stable plan fingerprint and sorted exact impact scopes.

For reflected FText, the common resolver and typed value overlay preserve the
captured canonical value when an ordinary source string is unchanged. A changed
source remains an edit intent; explicit localized input retains its identity
fields. Native identity resolution belongs to the rollback-capable property
apply stage, not planning. See [the shared value contract](../UObjectPropertyValueV2.md).

## Initialization graph

Every declared node binds a stable local identity, role, type identity, and one initialization-contract identity. Edges are one of:

- ownership: one lifecycle owner per child and no cycles;
- creation dependency: deterministic ordering before identity creation;
- reference: deferred until identities exist, with shared references and cycles permitted.

Ownership and creation dependencies are ordered with an explicit iterative worklist. There is no artificial nesting-depth limit and no recursive traversal dependency on the C++ call stack. Duplicate nodes, duplicate edges, missing endpoints, multiple owners, ownership cycles, and creation-dependency cycles fail before plan publication. Reference edges remain in the immutable plan for the later binding phase.

## Domain provider registry

`FImportInitializationDomainProviderRegistry` freezes an explicit provider set before execution. Provider identity must be exact and unique. A request carries the public command, schema, target, root type, structural role, initialization-contract identity, and canonical parameters; resolution requires one and only one provider to match the request and support that exact role/contract pair.

No match returns `hjson_domain_provider_unresolved`, duplicate provider identity fails freeze with `hjson_domain_provider_duplicate`, and more than one runtime match returns `hjson_domain_provider_ambiguous`. The process factory catalog seals on first freeze and rejects late registration. Only the resolved `IImportInitializationDomainProvider` can enter the domain execution port; handlers and domain adapters may not add fallback type branches or copy common graph traversal.

## Resource and failure semantics

The planner records node, item, reference-edge, work, and memory usage for the whole request. These counters do not impose admission limits or reject work. Directory, individual-file and virtual-bundle workspace loading have no imposed file-count or byte budget. Structured HJSON parsing/writing does not stop at estimated memory, elapsed-time, or output-size thresholds. Owned-object property projection has no imposed property-count limit. The complete graph is processed rather than truncated to a budget.

Other stable failures cover invalid request/change/node/edge/impact, target revision read or conflict, unsupported changes, invalid or changed candidates, ownership/dependency cycles, and failed exact-impact authorization. Provider-owned errors remain intact when supplied. Successful output records the source model fingerprint, exact target revision, explicit changes, deterministic creation order, deferred references, resolved exact candidates, impact scopes, and plan fingerprint.
