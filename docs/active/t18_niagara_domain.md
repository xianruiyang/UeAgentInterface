# T18 Niagara HJSON domain

`niagara-domain-provider` is the sole `content_graph.*` domain provider for
`/Script/Niagara.NiagaraSystem`, `/Script/Niagara.NiagaraEmitter`, and
`/Script/Niagara.NiagaraScript`. The import schema is
`uai.niagara.edit/1`.

The public SDK calls are `client.niagara.module_candidates(...)` and
`client.content_graph.import_workspace(...)`. Use
`client.query.command_search(...)` and `client.query.command_detail(...)` to
inspect their live contracts. The CLI equivalent is `uai-cli exec`, which
forwards the same exact generated binding without a task-local helper. No
legacy Niagara command or tutorial registry is an ingress or fallback.

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

`niagara.module.candidates` currently accepts one persisted Niagara System,
`target_usage`, and the exact stage discriminators needed by that System:
`target_usage_id` and, for an emitter-owned stage, `emitter_handle_id`. The
owner builds a data-processing-only `FNiagaraSystemViewModel`, finds the exact
`UNiagaraStackScriptItemGroup`, and calls that group's native
`GenerateAddActions`. This is the same Add Module action set used by the Stack
UI; it does not open an asset editor, preview, or window.

The result separates search coverage from creation coverage. Every native
action is returned with compact `id`, display `name`/`label`, `action_kind`,
category, support state, and supported operations. Asset modules are currently
executable. Scratch-pad modules, New Scratch Pad, assignment/set-specific, and
wizard actions remain searchable but explicitly unsupported for HJSON creation;
`coverage_complete` can therefore be true while
`creation_coverage_complete` is false. Dynamic inputs are a different native
creation surface and are not part of this command.

HJSON copies only the selector into the module entry:

```hjson
provider_extension: {
  stages: {
    "system|SystemUpdateScript|<usage-guid>": {
      target: {
        usage: SystemUpdateScript
        usage_id: "<usage-guid>"
      }
      modules: {
        native_asset_module: {
          candidate: { id: "candidate:..." }
        }
      }
    }
  }
}
```

`candidate` minimally contains `name` or `id`; if both are present they must
resolve to the same action. Optional richer fields are assertions, not a second
identity. Stage, emitter, ordering, operation, inputs, and placement stay
outside `candidate`. The server resolves the exact stage and re-enumerates the
same native Add Module action set during preflight and again immediately before
the first write. A supported asset-module candidate is then executed through
the same native `ExecuteAddAction`; callers never copy a script path, binding
revision, or complete search row. Missing, ambiguous, context-mismatched, or
unsupported actions fail before mutation.

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
