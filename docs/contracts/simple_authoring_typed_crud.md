# Simple Authoring typed CRUD contract

## Scope

Simple Authoring exposes typed UE mutations through the shared public command
edge for domains that are not yet fully covered by the HJSON authoring path.
The production families are:

| Family | Public namespace | Distinct responsibility |
|---|---|---|
| WorldSimpleEffect | `world` | World, Data Layer, streaming, HLOD and partition operations |
| LandscapeGeometryEffect | `landscape` | Landscape creation and height data operations |
| ModelingGeometryEffect | `mesh` | Modeling topology, geometry, UV, normal and collision operations |
| AssetDataSimpleEffect | `asset` | Asset lifecycle and asset-type structures not yet covered by HJSON |
| EditorProjectSimpleEffect | `editor_project` | Project configuration, settings and string-table operations |

The exact command IDs, Python methods, parameter names and wire types are the
current entries exported in `Resources/UaiCommandManifest.json`. The manifest
is a deterministic projection of the production C++ registry; documentation
and Python code do not maintain a second command list. A family remains public
only while its necessary inputs, lifecycle or result semantics are not already
covered by the generic HJSON chain; verified overlap migrates out instead of
leaving two active entry points.

## Caller flow

The public caller performs only these steps:

1. use the active domain Query/candidate command to select a canonical target
   and obtain any required business values and field identities;
2. call one generated typed SDK method with the command's declared parameters;
3. inspect the returned operation outcome and structured error;
4. use the active domain Query/readback command to independently confirm the
   resulting state when the task needs an external readback.

There is no public `reference.catalog_search` command. Command discovery uses
`query.command.search` followed by exact `generated SDK 与正式 schema`; target and
field discovery stays with the active domain owner instead of a second global
target directory.

For AssetData and EditorProject update-style commands, `target_ref` identifies
the selected target and `changes[]` contains typed field changes. The server
freezes the current target revision in hidden evidence during exact target
resolution; later prestate and prewrite checks reject a target that changes
within that operation. A change states the field identity, presence and typed
canonical value. Create, delete and domain operations expose only their
compiled Descriptor inputs; callers do not construct revisions, sessions,
bindings, tickets, plans, execution receipts, persistence artifacts or claimed
readback.

Python uses one import and one connection context:

```python
import uai

with uai.connect("connection.toml") as client:
    result = client.editor_project.project_settings_update(
        changes=changes,
        target_ref=target_ref,
    )
```

The SDK forwards the generated command over the configured transport. It does
not implement UE business rules or a second authoring path.

## One runtime path

Every active typed command follows the same production composition:

```text
public command edge
  -> exact command contract and thin handler
  -> immutable family profile
  -> exact target/reference resolution
  -> C3 operation application
  -> C6 plan
  -> C7 GenericEffect execution
  -> C8D durable persistence
  -> C9 independent persisted readback
  -> C10 outcome
```

Command handlers only decode the public parameters and select the exact
immutable profile bound at startup. They do not choose mutation mechanics by
string, save files, synthesize verification results or fall back to legacy
handlers.

`world.actor.create` is a dormant compatibility command. New persistent Actor
creation uses `world.actor.candidates` followed by Level Actor HJSON
`content_graph.import_workspace`; the provider extension owns label, folder,
transform, attachment and non-reserved Actor tags. Factory-backed classes such
as Volume actors must use the returned candidate rather than a guessed class
path so the native placement factory keeps its required construction semantics.

In the exported `actor_component_hierarchy.hjson`, add an entry under `actors`.
The map key names the new instance; the row's top-level `name` is the exact
readable name returned by `world.actor.candidates`. For example, the row shape
is `NewActor: { name: "<returned candidate name>" }`. Do not replace this with
`class` or a nested `candidate` object. Existing actors retain their exported
keys; edit their exported `label` or `transform` fields as needed.

`complete` requires durable acknowledgement and a semantic match from the
independent persisted readback. A mismatch is `partial`; unavailable durable
state is `state_unknown`. A successful transport envelope is not proof of a UE
state change.

Verification failures keep their stable public codes. An unavailable oracle
returns `verification_oracle_unavailable`; the Result error's `diagnostic.cause`
includes the exact oracle stage and cause code, plus any recognized structured
underlying diagnostic. The command boundary publishes actionable ordinary failure
reasons through the common runtime diagnostic; explicitly private context and machine
identities remain private. Nested carriers keep their own cause codes and source facts.
A semantic mismatch returns `verification_semantic_mismatch` with
`diagnostic.difference_count` and per-target `differences` containing kind,
path, left and right values from the same canonical comparison used by successful
verification views. Each target includes its business kind, role and structured
selectors. Snapshot paths use comparison-local entry indices, for example
`/entries/0/value/triangle_count`, rather than encoded internal identities. All
reported differences are retained; typed business values remain intact while
identity/revision custody stays private.
These diagnostics explain the failed readback but never authorize an automatic
replay of the write.

Ordinary workspace apply uses the same comparison facts in its authoring
diagnostic: leaf differences expose `expected_value` and `actual_value`, and
transaction/rollback outcomes survive domain and workspace projections. Direct
input locations use `source_kind: authored_value`; a value inferred from graph
connections uses `related_input` with the actual node/connection source locations
and its original `semantic_pointer`. A related source is navigation, not proof
that the user supplied the inferred value.

Producers declare a concise reason and the necessary field, expected/actual value,
presence or candidate facts when they are known. Internal dispatch/custody context
is not a substitute for a public cause. Higher layers forward the complete
diagnostic; a distinct wrapper retains its cause and facts. Native APIs that only
return failure cannot supply an invented lower-level explanation.

File navigation comes from the original parser provenance, including referenced
HJSON fragments. `json_pointer`, line and column identify the same physical input.
If only an existing ancestor is available, it is `related_input`; the requested
leaf remains in `semantic_pointer`. Runtime-only failures do not invent a source
file or line. Field and index context is added where the owner still has the
structured input, not reconstructed from a diagnostic message.

## Target, field and revision semantics

- Catalog candidates expose readable canonical selectors.
- Exact resolution rejects ambiguous, missing or incompatible targets.
- AssetData and EditorProject operations capture their target revision once
  during exact resolution. The captured machine value is not a public
  parameter or business result and is not copied into expected post-write
  state.
- C9 re-resolves the persisted target after C8D. It must not reuse the live
  object, pre-write snapshot, handler cache or caller-provided value.
- World C9 consumes only the package artifacts declared for its verification
  target. It loads the receipt's map package and each declared External Actor
  package from disk into isolated transient package names, reads the canonical
  state there, and unloads that isolated set. It does not enumerate or reuse
  the live Editor World or live Actor instances; a missing, duplicate or
  unloadable declared package makes independent readback unavailable.
- Editor Project settings/config readback uses durable config-file state rather
  than the mutable global config cache.
- Legal canonical values have no policy-defined nesting-depth limit. Resource
  budgets still bound bytes, nodes and execution work.

### Modeling carrier revisions

Modeling commands that edit an existing carrier accept the business
`target_ref`; operations with a second mesh additionally accept
`operand_ref` and its transform. They do not expose `target_revision` or
`operand_revision`.

The ModelingGeometryEffect owner resolves the exact `ADynamicMeshActor` or
`UStaticMesh`, derives the current carrier revision and geometry fingerprint,
and freezes them in the same operation's hidden planning/binding evidence. For
binary operations it does the same for the resolved operand. Native checkout
re-resolves both carriers and compares the frozen fingerprints before the first
write; changed targets fail closed, while persisted readback and rollback retain
their existing responsibilities.

These revisions cover transform, geometry and topology,
UV/normal/material state, material slots and supported collision state. They
remain machine facts: the caller does not read, copy, guess, return or encode
them in a target name. Element `selection` still contains only the business
`element_kind` and `element_ids`. Other Actor, Component, Graph and Viewport
targets retain their own revision owners and are not interpreted as Modeling
carriers.

Public apply failures preserve their primary structured diagnostic and add transaction
facts to `error.diagnostic`: `transaction_status`, `rollback_attempted`,
`rollback_succeeded`, and `retry_safe`. These fields come from the shared
coordinator. A failed rollback or unknown state never becomes retry-safe merely
because the domain's original error was transient. Public causes, values and source
locations remain intact; internal details are held separately at the producer.

## Persistence ownership

C8D owns persistence for every registered family. Package-backed content is saved as
an owned package artifact; Editor Project profiles use their declared project
config artifact. Domain hooks mutate UE state but do not independently save or
delete durable artifacts.

The durable target set must contain every package actually created or changed
by the mutation, including packages allocated after preflight such as External
Actor packages. A command cannot report `complete` merely because the World
package was persisted while a dynamically created package remains dirty.
Mutation facts may extend only the prepared request's physical artifact set:
each added package must still belong to a frozen planned change, preserve that
change's save/delete intent, pass the current fence and allowed-root checks,
and receive its backup and journal entry before persistence starts. The same
C8D session, journal, transaction root and backend then stage, publish or
restore the combined inventory. This does not scan or absorb unrelated dirty
packages.

`world.actor.create` and `world.level_game_mode.update` no longer expose the
legacy `save_level` parameter. The former remains a durable dormant compatibility
operation; the latter remains active. Neither command offers a transient boolean
mode. Their former machine contract identities remain internal audit facts and
are not copied into model-facing documentation or requests. Callers remove the
argument; submitting the old field is rejected by `additionalProperties:false`
rather than silently ignored. A genuinely transient World mutation would
require a separate lifecycle contract and is not provided by these commands.

Consequently, the following standalone legacy write/save commands are retired
and have no exact or prefix invoker:

- `save_current_level`
- `level_streaming_save`
- `modeling_save_mesh_asset`
- `level_topology_set_actor_assignments`
- `config_apply_section`
- `project_settings_apply_page`
- `device_profile_apply_json`
- `localization_apply_json`
- `platform_config_apply_json`
- `scalability_apply_json`
- `string_table_apply_json`

There is no alias, dual write, fallback or compatibility route for these IDs.
Lifecycle command parameters that happen to use names such as
`save_current_level` are separate parameter contracts and do not reactivate a
standalone command.

## Editor Project mapping

Editor Project write commands cover configuration sections, device profiles,
localization targets, platform configuration, project settings, scalability
groups and string tables. Callers obtain the current exact command identity
through keyword search and detail rather than this document maintaining a
second command inventory.

Callers select the target through the relevant active Editor Project Query
surface, then request its exact write metadata with the closed
`editor_project_capability: "target.read"` action. Queries for configuration,
project settings, platform state and validation route `target.read` to the same
production Editor Project resolver. The observation contains readable
target/profile information; project-settings targets also contain exact
`fields[]` identities, type IDs and current canonical values, while
config-backed targets contain independently loaded `entries`. Machine
revisions stay inside the server-owned operation evidence.

The caller then submits the matching generated update method and uses the same
Query route for independent readback. It must not provide a revision, restore
`reference.catalog_search`, or maintain a second target/field catalog. All
Editor Project write commands share the same authority, revision, C8D and C9 rules while
retaining their profile-owned fields and native UE mutation hooks.
