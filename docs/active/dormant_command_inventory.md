# Dormant command lifecycle and inventory

This development-only document records the centralized compatibility surface
for commands whose normal authoring responsibility has moved to an active HJSON
owner. It is not distributed in the `ue-agent-interface` skill.

## Lifecycle contract

Dormancy is one registry lifecycle state, not physical retirement and not a
per-command implementation technique:

- the original public command ID, frozen contract, unique C++ owner and exact
  handler remain registered and executable;
- normal `query.command.search` excludes dormant commands;
- `query.command.search(..., mode="compatibility")` includes active and dormant
  candidates and labels every candidate with `lifecycle_status`;
- a dormant candidate and its detail expose `successor_command_ids` and
  `lifecycle_reason`;
- generated named SDK methods are emitted only for active commands;
- `Client.execute`, `Client.execute_exact`, and `uai-cli exec` use the ordinary
  exact path for a known dormant command; `Client.execute_dormant` remains only
  as a compatibility alias for the same dispatch;
- disabled commands remain undiscoverable and non-executable.

The single data owner is
`Core/Contract/Command/UeAgentCommandLifecyclePolicy.cpp`. Adding or changing a
dormant identity updates that table and its shared projections; registrations,
handlers and search code must not carry local dormant lists.

## Current dormant commands

The exact inventory is the `status="dormant"` projection in
`Resources/UaiCommandManifest.json`; this document deliberately does not copy the
ID list into a second hand-maintained table. The central policy currently covers
these responsibility groups:

- superseded World Actor discovery, structure, property, transform and creation;
- persistent asset properties, structure and domain-specific readback now owned by
  HJSON export and the ContentGraph query family;
- typed asset, Animation, Rig, Physics, Timeline, PCG, Niagara and TextureGraph
  desired-state mutation;
- persistent World Data Layer, streaming, HLOD, GameMode and relation mutation;
- localization-target and StringTable mutation through registered HJSON profiles;
- the registry-only relation compatibility view superseded by `asset.relations`.

Some HJSON successor profiles or semantic adapters are not yet complete. Those are
explicit capability gaps: the old typed identity stays dormant and must not be used
silently as the normal workflow. Procedural geometry, CubeGrid/voxel sessions,
whitebox construction, algorithms, external import, runtime queries, validation,
capture, compile/settle and Editor/Operation lifecycle commands remain active.

`world.actor.create` remains executable only through the compatibility path. The
active flow searches `world.actor.candidates`, writes the selected readable name
and only necessary short semantic selection fields into Level HJSON, then imports
the current workspace. The same provider owns label, folder, transform, attachment,
tags, properties, components, persistence and independent readback.

Historical commands already removed from the current registry are not dormant
and must not be restored merely to populate this compatibility surface.
