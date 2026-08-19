# T37 AssetSimple Operation Authority

The active `asset-simple-operation` contribution owns exactly the 52 command
identities assigned to `T37-ASSET-SIMPLE-OPERATION` in
`UAI_COMMAND_TASK_INVENTORY.json`. Every frozen contract reports
`asset-simple-operation-owner`; no namespace or catch-all fallback adds commands.

The exact handler requires `uai:operate`, grants only the AssetData mechanics
scope declared by this adapter, and forwards one request to the
process-owned SimpleAuthoring Operation application. That lazy host creates at
most one runtime after the Editor world exists and survives transport or
CommandService reconstruction. The existing AssetData simple-effect code is an
internal UE-mechanics dependency only; it is no longer a public route, registry,
handler, contract, test, or documentation authority and is physically considered
for removal only by `T90` after all consumers have exited.

This scope mapping is not destructive authorization and cannot authorize a
delete, move, cleanup or any command outside the exact 52-command slice.

Success and failure remain command-specific. The common application performs
schema validation, planning, ticket binding, native execution, settle,
persistence, independent projection/readback, rollback or `state_unknown` as
required by the selected descriptor. A prewrite rejection has no residue;
`state_unknown` is not safe for automatic replay. A successful response exposes
the stable `operation_id` and canonical `projection`; the latter carries the
verified status, evidence identities and persisted/current-state readback.

## Public SDK and CLI

The final manifest projects all 52 identities into the default `asset`
namespace. Callers import only `uai`, connect from one complete Connection TOML,
and call methods such as `client.asset.sound_class_create(...)` and
`client.asset.render_target_create(...)`. Generic `client.execute(...)`, exact
`client.execute_exact(...)`, and `uai-cli exec` reuse the same generated catalog,
Client, Protocol, and Transport. No T37-specific Python module, command/hash
table, parameter mapper, retry, persistence, rollback, or readback path remains
authoritative.

```text
uai-cli --connection connection.toml exec asset.sound_class.create --params-json {"target_ref":"/Game/StrictV2/T37/SC_Default.SC_Default"}
```

Use `client.query.command_search("create sound class", limit=5)` and then
`client.query.command_detail(command_id, contract_hash)` to obtain the exact
schema, named SDK/CLI forms, owner, failure catalog, and tutorial references.
Search is the only fuzzy step; execute the selected ID and hash unchanged.
