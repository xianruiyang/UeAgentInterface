# T60 HJSON external-input domain

## Authority and composition

T60 contributes `hjson-external-input-domain-provider` to the process-frozen
HJSON domain-provider registry. It does not register another public command or
dispatch. SDK and CLI callers continue to use the T13-owned
`content_graph.import_workspace` and `content_graph.export_workspace` exact
bindings. The SDK exports with `client.content_graph.export_workspace(...)`
and imports edited ordinary files with the returned `workspace.import_changes()`.
Standalone HJSON uses `client.content_graph.import_file` or `import_inline`;
the exact server command `content_graph.import_workspace` is not a directly
callable SDK method. Callers discover commands through
`client.query.command_search(...)` and consult current SDK signatures and the
formal schema for the source carrier.

No task-local projection helper or second command catalog remains active.
All family, digest, target-policy, and importer validation is owned by the
server provider. Preparation and ordinary editing use the common semantic
workspace composition, hidden inventory and edited-workspace resolver.

The `uai.hjson.external_input/1` document owns only the external source
identity, format, import options,
and an explicit target policy. The target policy names exactly one of the 13
T28-T30 importer commands and carries only that command's target parameters.
Unknown families or importer commands fail before delegation.
Pass a complete document as the optional `source` of `export_workspace` to
prepare a workspace without running an importer. The server reads the external
file and keeps its exact path and digest in the hidden semantic envelope.
The ordinary files contain no SHA or option fingerprint. Options declared by the selected importer may be edited;
changing the source identity requires preparing a new workspace. A source byte
change after preparation is rejected before delegation. Relative source paths
are resolved against the UE project directory.
The target named by the public content-graph request must equal the delegated
`destination_ref` or `target_ref`; a document cannot redirect a validated
request to a second target.

Factory options use the selected command's native option record. Skin Weight
Profile and Localization currently declare no additional import options, so
their `options` record is empty; profile name and LOD remain Skin Weight target
parameters. Undeclared option fields produce field diagnostics before native
mutation rather than being ignored. Integer counts and indices retain their
original numeric meaning through the common codec.

## Initialization and import

Factory, Skin Weight Profile, and Localization each have one provider-owned
normal-creation initialization contract. T13 is the sole common-HJSON
authority: it owns source-carrier resolution, initialization, recursive
traversal, provider/default resolution, binding, failure, rollback, and cleanup
semantics through the frozen registry. T60 consumes T13's normalized sources
and public semantic workspace resolver, then contributes the
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

The registered T60 delegation surface includes all 13 T28-T30 commands and
retains importer-owned persistence, independent readback, failure and cleanup
semantics. Registration and the fixed-success gateway test do not establish
real HJSON consumer validation. The Factory exporter accepts object-backed
results whose persisted class and option fingerprint identify a registered
route; re-import still requires the original source file and matching hidden digest.
Prepared object-backed workspaces reject a changed persisted target or a dirty
target before import. A fresh inline/single-file input is create-only and obtains
its source digest on the server during preparation for that call; it carries no
cross-call source baseline. Use an exported workspace when that baseline is needed.
Skin Weight Profile
and Localization remain import-only in T60 because their current public target
identities do not provide the same general asset-provenance export contract.
This is an explicit unsupported surface, not a fallback or inferred export.

Historical HJSON parser branches and historical external-import public owners
are not fallbacks. T60 delegates only to the current T28-T30 command services;
T44 removed the old compiled registry/HTTP handler and isolated obsolete public
composition seams after zero-active-reference verification.
