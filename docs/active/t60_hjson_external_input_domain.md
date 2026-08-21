# T60 HJSON external-input domain

## Authority and composition

T60 contributes `hjson-external-input-domain-provider` to the process-frozen
HJSON domain-provider registry. It does not register another public command or
dispatch. SDK and CLI callers continue to use the T13-owned
`content_graph.import_workspace` and `content_graph.export_workspace` exact
bindings. The default SDK ingress is
`client.content_graph.import_workspace(...)` or
`client.content_graph.export_workspace(...)`; callers discover the current
contract through `client.command_search(...)` and
`client.command_detail(...)`, with the same `command_search` and
`command_detail` operations exposed by `uai-cli`.

No task-local projection helper or second command catalog remains active.
Callers author the versioned document and pass it unchanged through the default
binding; all family, digest, target-policy, and importer validation is owned by
the server provider.

The `uai.hjson.external_input/1` document owns only the external source
identity, format, SHA-256 digest, optional source information, import options,
and an explicit target policy. The target policy names exactly one of the 13
T28-T30 importer commands and carries only that command's target parameters.
Unknown families or importer commands fail before delegation.
The target named by the public content-graph request must equal the delegated
`destination_ref` or `target_ref`; a document cannot redirect a validated
request to a second target.

## Initialization and import

Factory, Skin Weight Profile, and Localization each have one provider-owned
normal-creation initialization contract. T13 is the sole common-HJSON
authority: it owns source-carrier resolution, initialization, recursive
traversal, provider/default resolution, binding, failure, rollback, and cleanup
semantics through the frozen registry. T60 consumes only T13's normalized
`source.kind=resolved` document set and entry identity, then contributes the
external-input document projection and family validation. The T60 provider
verifies the external payload digest before write and projects a canonical
importer request, then calls the existing T28, T29, or T30 production command
service. It does not add a second carrier loader, recursive walker,
initialization path, importer business layer, factory/profile/Localization
mechanic, persistence path, rollback path, or independent readback path.

The delegated terminal response is the public import result. The underlying
importer remains the sole authority for mutation, stabilization, save,
unload/reload, business-state verification, failure classification, rollback,
and cleanup.

## Export, readback, and legacy exit

Object-backed factory imports support full export through the same
`content_graph.export_workspace` ingress. The exporter reads only the T28
provider's persisted source, digest, operation, option fingerprint, asset type,
and real UE dependencies. It reconstructs the exact supported Boolean option
set from the persisted fingerprint, writes source information, generated
assets and dependencies into the document, and publishes through the shared
verified-export transaction. Re-importing that published document uses the
same T60 provider and the same T28 command service.

The declared T60 production surface is therefore: all 13 T28-T30 commands are
available for HJSON import with their importer-owned persistence, independent
readback, failure and cleanup semantics; full HJSON export/import round-trip is
available for T28 object-backed factory results whose persisted class and
option fingerprint identify one registered factory route. Skin Weight Profile
and Localization remain import-only in T60 because their current public target
identities do not provide the same general asset-provenance export contract.
This is an explicit unsupported surface, not a fallback or inferred export.

Historical HJSON parser branches and historical external-import public owners
are not fallbacks. T60 delegates only to the current T28-T30 command services;
T44 removed the old compiled registry/HTTP handler and isolated obsolete public
composition seams after zero-active-reference verification.
