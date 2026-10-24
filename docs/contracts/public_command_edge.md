# Public Command Edge Contract
## Purpose

The public command edge is the transport-neutral entry used by generated
clients and the thin CLI. It accepts one exact `uai.command.v1` request and
forwards it through the process-active immutable C++ registry to one exact
handler. It does not expose internal descriptors, provider bindings, route
selection, sessions, tickets, or business defaults.

## Ownership

| Concern | Owner |
|---|---|
| process capability registration and one frozen authority snapshot | `DomainCapability` |
| command IDs, field shape, availability, side effects, purpose, errors and contract hash | domain command declarations frozen by `FUeAgentImmutableCommandRegistry` |
| formal SDK projection | `FUeAgentActiveCommandManifest` → `Resources/UaiCommandManifest.json` |
| server request decoding, request validation and result encoding | `FUeAgentCommandProtocolCodec` / `FUeAgentCommandIngress` |
| public client response decoding | Python SDK `uai.uai_protocol` |
| exact command-to-handler mapping | `FUeAgentExactCommandDispatch` |
| transport-neutral service lifecycle | `FUeAgentCommandService` |
| HTTP route, authentication and adapter lifecycle | `FUeAgentInterfaceModule` and the concrete transport adapter |
| Query, authoring, operation and readback behavior | the exact domain handler and its application/mechanics owner |

Active composition and HJSON provider projection have no independent process
catalog. Module startup obtains the one cached DomainCapability authority and
publishes the command route from that runtime registry. The committed manifest
is a rebuildable SDK projection; a missing or stale projection does not prevent
service startup or ordinary exact command execution.

## Transport binding

- wire schema: `uai.command.v1`
- transport channel: `uai.command`
- current HTTP adapter route: `POST /api/uai/command`
- authentication header: `X-UeAgentInterface-Token`

HTTP is only an adapter. The registry, protocol, exact dispatch, and domain
handlers do not select HTTP routes.

## Request and result

The public SDK accepts a command name and `params`; transport envelope fields,
dispatch identities, correlation values, and contract fingerprints are
server-owned machine facts and are not model-facing inputs or outputs. Unknown
public parameters are rejected. Omitted optional parameters remain omitted;
explicit `null` remains explicit. JSON numbers keep their original lexeme
through protocol parsing.

A `command_result` preserves request identities and contains `ok`,
`payload`, and `error`, plus an optional `diagnostics` array of complete public
diagnostics independent of the business payload. This array is omitted when
empty and does not change `ok` or the operation's terminal status. SDK domain
projections and CLI output preserve every diagnostic field. A successful envelope has an object payload and
`error=null`; a failed result preserves the server payload and structured
error. Envelope, registry, binding-hash, parameter, caller, and result-identity
failures return `protocol_error`. Both failure kinds carry the producer's complete
public `error.diagnostic`, including nested causes and input locations; the SDK
does not reconstruct a protocol diagnostic from a message-only error. An `ok=true` envelope alone does not prove
that a write persisted or that independent readback matched.

Query projection and catalog diagnostics use this top-level array. The old
typed/legacy query payload `projection.diagnostics` and `catalog.diagnostics`
output paths are retired; catalog completeness remains in the business payload.

Internal failures retain their typed `FDiagnostic` through dispatch and
aggregation. Public diagnostic JSON is a one-way projection at the output
boundary; internal consumers do not reconstruct diagnostics from that JSON or
from diagnostic strings. The server codec has no result decoder. Client wire
decoding remains responsible for validating actual received responses.
The codec's `TryProjectResult` owns this structured result projection for both
wire encoding and deferred operation signals. Deferred signals consume the
projected object directly; they do not encode and reparse a wire result.

Value comparisons create structured differences at the comparison source.
Recursive parents prefix the selected field or collection path; downstream
consumers preserve the path, necessary expected/actual values and explicit
presence flags, adding only context and source locations. Type-specific numeric,
rotation and collection equivalence rules still belong to their comparators.
Typed comparison producers construct public values separately from internal
identity/revision evidence. Serialization preserves public business keys verbatim,
including names such as `identity` and `hash`; it neither filters keys nor replaces
differing composite values with identical kind/count summaries or compares them again.
An absent value is distinguished from explicit JSON null. Workspace conflicts
retain the proposed value at the same path, or a separate structured proposed-edit
difference when the edits concern different paths. A semantic path without a
proven file binding is not presented as an exact HJSON location.

## Discovery boundary

Command discovery is owned only by:

- `query.command.search(intent)`
- `query.command.usage(commands)`
- `generated SDK 与正式 schema(command, variant?)`

Search returns the complete deduplicated array of matching canonical command names.
Generic contracts, synonyms, and variants contribute search tokens to the same
parent command. Usage accepts any nonempty list of
names and returns a name-to-minimal-named-Python-call map containing only
required top-level arguments. Detail is exceptional: it returns full purpose,
schema, errors, and tutorial references for one command and optional readable
variant. Variant selects one request shape and never creates a second command.
Machine bindings, hashes, raw wire identities, ranking reasons, and pagination
state are not part of any model-facing command query. There is no public
`reference.catalog_search`; target, field, node, component, Actor, and other
content candidates remain with their applicable domain Query owner.

## Structured workspace commands

The model-facing SDK projection of `content_graph.export_workspace` accepts:

- required `target:string`;
- optional short `name:string`;
- optional `profile_id:string`.

The SDK derives the absolute wire `workspace_path`, privately retains it, and
returns an `HjsonWorkspaceHandle` with a short project-relative `.path`. The
server performs the fresh content read, semantic projection and atomic
workspace publication. Callers do not assemble internal read sections,
provider routes, absolute paths, or manifest identities.

The raw wire form of `content_graph.import_workspace` accepts:

- required `target:string`;
- required `source:object`.

Model-facing directory edits use `HjsonWorkspaceHandle.import_changes()`;
direct path-bearing import is hidden by the normal SDK namespace. Internally,
`source.kind` is `directory`, `single_file`, or `inline`. File-backed
sources carry `path`; inline sources carry the document specified by the
current HJSON contract. Domain intent is expressed by that document, not by a
client-side revision, authorization, route, `intent`, or `dry_run`
compatibility parameter. The C++ server restores machine identity and revision
from the hidden workspace manifest and owns parsing, planning, validation, mutation, compile/settle,
persistence, independent readback, failure state and cleanup.

Imports are accepted on the existing long-running operation owner before deferred
dispatch. The wire reply contains an operation reference; its terminal signal
`uai.command.result.v1` preserves the exact final command reply, including business
failures. The normal SDK drives this operation and still returns the final import
`Result`. After the acceptance reply has supplied the operation handle, a transport
timeout retains SDK custody and reports a recovery name;
`client.content_graph.resume_import(name)` collects that same operation's result
without resubmitting the mutation. Recovery belongs to the same Editor session;
an Editor restart is not permission to replay an unconfirmed import. If the first
acceptance reply itself was not received, no recovery handle is available; check
the actual target state before deciding the next action.

## Manifest boundary

`Resources/UaiCommandManifest.json` is the deterministic client projection of
the active immutable registry. Its source kind is
`cpp_immutable_command_registry_export`. It contains only:

- command ID, namespace, method and active status;
- exact parameter name/type/required/nullable shape;
- supported wire range and binding hash;
- registry hash and exporter provenance.

It does not contain descriptor identities, HTTP routes, business defaults,
success rules, domain schemas, or Python business logic. Generated `.py` and
`.pyi` bindings consume only this manifest and are checked rather than
hand-maintained.

## Evidence boundary

The active registry, exact dispatch and generated manifest define the command-edge
structure without an omitted-contribution fallback. Each domain still
requires its own public ingress, real UE mechanics, persistence/readback,
failure and cleanup evidence before its capability can be called complete.
