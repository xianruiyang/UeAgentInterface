# T38 ModelingGeometry Operation Authority

The active `modeling-geometry` contribution owns exactly the 32 mesh attribute,
collision, primitive, topology and Modeling Mode command identities assigned to
`T38-MODELING-GEOMETRY-OPERATION` in `UAI_COMMAND_TASK_INVENTORY.json`. Every
frozen contract reports `modeling-geometry-owner`; no namespace or catch-all
fallback adds commands.

The exact handlers require `uai:operate`, add only the private
`uai:modeling-geometry:write` mechanics scope, and forward one request to the
common OperationService. The ModelingGeometryEffect adapter remains the sole
owner of the real Unreal Engine geometry differences. It is an internal
mechanics dependency, not a second public route, registry, handler, contract,
test or documentation authority.

Success and failure remain command-specific. The common lifecycle performs
schema validation, planning, ticket binding, native execution, settle,
persistence, independent readback and rollback or `state_unknown` handling as
required by the selected descriptor. No success is emitted before all required
stages finish. A prewrite rejection leaves no residue, and `state_unknown` is
not safe for automatic replay.

`mesh.modeling_session` is the single public successor for Modeling Mode
lifecycle, active-tool observation, tool properties/actions, mesh selection,
save, actor-mesh replacement and surface snapping that are not already owned by
an exact current mesh command. Its required `action` discriminator is one of
`activate`, `observe_tool`, `start_tool`, `set_tool_property`,
`invoke_tool_action`, `accept_tool`, `cancel_tool`, `observe_selection`,
`set_actor_selection`, `set_selection_mode`, `clear_mesh_selection`,
`select_via_screen`, `select_via_world_ray`, `replace_actor_mesh`,
`save_mesh_asset`, or `snap_to_surface`. The optional `target`, `input`,
`context`, and `projection` objects carry only action-specific data. Unknown
actions and missing required action inputs fail explicitly; historical
`modeling_*` IDs are not aliases.

## Public SDK and CLI

The final manifest projects all 32 identities into the default `mesh`
namespace. Callers import only `uai`, connect from one complete Connection TOML,
and call generated methods such as `client.mesh.primitive_box_create(...)` and
`client.mesh.topology_mirror(...)`. Generic `client.execute(...)`, exact
`client.execute_exact(...)`, and `uai-cli exec` reuse the same generated catalog,
Client, Protocol and Transport. No T38-specific Python module, command/hash
table, parameter mapper, retry, persistence, rollback or readback path remains
authoritative.

```python
result = client.mesh.modeling_session(
    action="observe_tool",
    target={},
    input={},
    context={},
    projection={},
)
```

```text
uai-cli --connection connection.toml exec mesh.primitive.box.create --params-json {"anchor":"base_center","placement":{"semantic_frame_id":"uai.world","write_frame_id":"ue.world","read_frame_id":"ue.world","verify_frame_id":"ue.world","unit":"centimeter","translation":[125,-50,30],"rotation_quaternion_xyzw":[0,0,0,1],"scale":[1,1,1],"basis_evidence":"UE left-handed Z-up centimeters","conversion_chain":"identity"},"size_cm":[120,80,60],"subdivisions":[2,2,1],"target_ref":"UAI/StrictV2/T38/Box_Default_r58"}
```

Use `client.query.command_search("create mesh box", limit=5)` and then
`client.query.command_detail(command_id, contract_hash)` to obtain the exact
schema, named SDK/CLI forms, owner, failure catalog and tutorial references.
Search is the only fuzzy step; execute the selected ID and hash unchanged.
