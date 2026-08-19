# T18 Niagara HJSON domain

`niagara-domain-provider` is the sole `content_graph.*` domain provider for
`/Script/Niagara.NiagaraSystem`, `/Script/Niagara.NiagaraEmitter`, and
`/Script/Niagara.NiagaraScript`. The import schema is
`uai.niagara.edit/1`.

The public SDK calls are `client.niagara.module_candidates(...)` and
`client.content_graph.import_workspace(...)`. Use
`client.command_search(...)` and `client.command_detail(...)` to discover and
inspect their live contracts; `uai-cli` exposes the same discovery path. No
task-specific helper or tutorial-owned registry is authoritative.

The provider resolves Niagara root, stack, module, input, and parameter
initialization only through the common T13 recursive registry contracts. T13
is the sole common source-carrier and recursive initialization authority; T18
consumes T13's normalized `source.kind=resolved` document set and contributes
only the Niagara domain contracts:

- `uai.init.niagara_system.root/1`
- `uai.init.niagara_emitter.root/1`
- `uai.init.niagara_script.root/1`
- `uai.init.niagara.stack/1`
- `uai.init.niagara.module/1`
- `uai.init.niagara.input/1`
- `uai.init.niagara.parameter/1`

It does not own a second recursive initializer, default-value decoder,
reference binder, compile loop, persistence path, or cleanup algorithm.
Unsupported root types and unsupported root-operation combinations fail before
mutation.

Root creation delegates to the authoritative T51 Niagara commands:
`niagara.system.asset.create`, `niagara.emitter.asset.create`, or
`niagara.asset.duplicate`. Existing-root imports contain one explicit
`root.operation` with an exact T51 command identity and params. Scripts accept
`niagara.script.graph.apply` and `niagara.compile_refresh_settle`; systems and
emitters accept `niagara.stack.apply`, `niagara.stack.reorder`,
`niagara.stack.issue_fix`, `niagara.compound.apply`,
`niagara.renderer_event_parameter.apply`, and
`niagara.compile_refresh_settle`. The domain adapter validates the root and
target coherence, then forwards the operation to T51 without copying its
mechanics.

`niagara.module.candidates` queries the real Niagara module catalog for the
persisted target and returns bounded, paged candidates with exact executable
`name`, `binding_revision`, input and placement contracts, and required
context. Display text is not executable. Imports must preserve the selected
candidate identity and revision; a missing, renamed, or stale binding is a
prewrite failure.

Queries capture the actual Niagara owner through the common content-graph read
environment and return stable revision, items, relations, properties,
parameters, diagnostics, and compile state. Imports require the observed
revision for existing roots, invoke the exact T51 command, and independently
recapture the persisted owner after reload. Creation requires the target to be
absent; an existing target fails with `niagara_domain_stale_revision` before
write.

T18 does not register a Niagara workspace-export serializer. The shared
`content_graph.export_workspace` command therefore fails explicitly with
`niagara_domain_export_not_registered` instead of returning a query snapshot
that cannot round-trip through the Niagara import schema. No legacy Niagara
owner, generic catalog selector, or parallel HJSON runtime is an ingress or
fallback for this domain.
