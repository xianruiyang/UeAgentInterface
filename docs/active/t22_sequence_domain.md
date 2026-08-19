# T22 Sequence HJSON domain

The only Sequence authoring ingress is
`client.content_graph.import_workspace(...)`, with
`client.content_graph.structure(...)` for independent readback. The shared SDK
also exposes `client.content_graph.export_workspace(...)`, but T22 does not
claim a lossless Sequence export projection; it returns
`sequence_domain_export_not_registered` instead of a false-success read
payload. Callers obtain current fields and hashes through `command_search` and
`command_detail`; `uai-cli ... exec <command-id>` exposes the same contracts.
No task-specific generated binding is active.

`sequence-domain-provider` is the sole HJSON domain provider for
`/Script/LevelSequence.LevelSequence` and schema `uai.sequence.edit/1`. It owns
the root contract `uai.init.sequence.root/1` and is selected by T13's frozen
provider registry; it does not register a second public command.

T13 remains the sole initialization, recursive traversal, default resolution,
binding, failure, rollback, and cleanup authority. This provider consumes that
contract and does not implement private recursion or initialization.

The provider accepts arbitrary valid HJSON through the common workspace parser,
then delegates Sequence mutation and independent readback to T55
Timeline/Sequence native mechanics. `root.structure` uses the shared exact-time
representation for display rate, tick resolution, playback range, bindings,
tracks, sections, channels, and keys. Missing targets require
`expected_revision: missing`; existing targets require the revision returned by
the previous independent structure or import readback. Validation completes
before delegated mutation.
