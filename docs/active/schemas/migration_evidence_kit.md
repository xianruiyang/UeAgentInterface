# Historical migration evidence kit

`uai.historical_successor.v2.schema.json` is the formal structure for an owner-authored historical
successor document. The shared Python adapter lives at
`UeAgentInterfaceCMD/tools/uai_repository/migration_evidence`.

The approved task inventory remains authoritative for every complete historical obligation row. The
owner supplies the exact assigned row set, successor and product evidence. The adapter loads the
owner's active rows from that inventory, fingerprints each complete row without a lossy projection,
and verifies that the successor document's approval/source/contract fields are an exact projection of
those rows. It does not infer a successor from a command name, prefix, namespace, nearby
implementation or task owner.

The normal owner flow is:

1. Load the complete active owner-row set from the approved
   `UAI_PRE_REFACTOR_COMMAND_TASK_INVENTORY.json` and compare its canonical fingerprint with the
   `historical-obligation:sha256:...` identity frozen in the task contract.
2. Build or load one `uai.historical_successor.v2` document with owner decisions for every row.
3. Load current command identities from `Plugins/UeAgentInterface/Resources/UaiCommandManifest.json`
   with `load_active_command_snapshot`; a caller-provided replacement set is not an authority.
4. Bind each distinct product `test_id` to an immutable receipt path, SHA-256, active registry hash,
   exact public command set and explicit JSON-pointer assertions.
5. Run `validate_historical_successor` with both the complete inventory rows and their frozen
   fingerprint. For an exact-command successor, semantic coverage names those commands; for an
   architecture-contract successor, semantic coverage names the exact contract identity while its
   representative commands and product receipt remain subject to current-manifest validation. The
   validator reports mechanical validity only; it does not change a
   disposition, runtime, task state or completion result.
6. Record the validated aggregate in the current capability catalog. Public-surface exit uses the
   v2 historical-disposition overlay plus current architecture/CMD oracles for old command,
   provider, handler, test, Gate and documentation identities. Historical evidence roots may be
   declared as allowed but never count as active; retired T61/T90/T99 task identities are not
   execution or completion authorities.

Changing any field of an assigned inventory row, its owner/lifecycle, the active command manifest or
a bound receipt invalidates the corresponding fingerprint/hash and requires the business owner to
review and regenerate its evidence. Hashing only the smaller successor-document projection is not a
valid substitute for the task contract's complete-row fingerprint. The shared adapter must not
silently update those identities.
