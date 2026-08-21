# T18 Niagara HJSON domain

`niagara-domain-provider` is the sole `content_graph.*` domain provider for
`/Script/Niagara.NiagaraSystem`, `/Script/Niagara.NiagaraEmitter`, and
`/Script/Niagara.NiagaraScript`. The import schema is
`uai.niagara.edit/1`.

The public SDK calls are `client.niagara.module_candidates(...)` and
`client.content_graph.import_workspace(...)`. Use
`client.command_search(...)` and `client.command_detail(...)` to inspect their
live contracts. The CLI equivalent is `uai-cli exec`, which forwards the same
exact generated binding without a task-local helper. No legacy Niagara command
or tutorial registry is an ingress or fallback.

## Import contract

The shared source loader owns inline, single-file, and directory carriers. The
domain consumes only its normalized `source.kind=resolved` document set. The
HJSON entry contains one exact root `type` and a `document` using the mature
Niagara authoring shape from `../hjson-authoring/08_NiagaraApplyDomain.md`.

T13 remains the sole common recursive initialization authority. T18 registers
only the Niagara root and owned-entry initialization contracts and consumes
that shared seam; it does not create another recursive dispatcher.

- A new root uses `expected_revision: "missing"` and
  `document.operation: create` or `clone`.
- An existing root uses the exact current `content_graph` revision and
  `document.operation: update`.
- `document.target.asset_path` must identify the public request target;
  `asset_kind` is normalized to `system`, `emitter`, or `script` from the exact
  root type.
- Compile and save are mandatory. Explicit `compile: false` or `save: false`
  fails before mutation.

The domain calls the single `niagara.apply` authoring provider for root
lifecycle, stages, modules, inputs, user parameters, renderer/event structures,
and recursive dynamic inputs. It does not copy the provider's parser,
transaction, rollback, compilation, persistence, or readback mechanics.

## Candidate-to-authoring chain

`niagara.module.candidates` queries the real module palette for one persisted
Niagara target and target usage. Each full candidate object contains exact
`name`, `kind`, operation/input/placement contracts, required context, and
`binding_revision`. Copy that object unchanged into the target stage module's
`candidate` field.

The candidate owner canonicalizes public usage tokens such as
`particle_update` and the corresponding UE stage identity
`ParticleUpdateScript` to one binding identity. The domain verifies the full
candidate against the current target and stage before converting it to the
internal script reference. Missing, renamed, contract-mismatched, or stale
bindings fail before write.

## Lifecycle and readback

Success requires the mature authoring provider to finish mutation, compilation,
semantic verification, and save. The domain then unloads the package, reloads
it through the common `ContentGraphRead` authority, requires
`compile_status=valid`, and returns the independent snapshot together with a
`reload:` persistence receipt.

`content_graph.export_workspace` remains explicitly unavailable until a
round-trippable domain export contract is registered. The replaced
`niagara.stack.apply` and `niagara.script.graph.apply` commands are inactive;
there is no legacy provider or parallel Niagara HJSON runtime.
