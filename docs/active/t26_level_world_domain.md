# T26 Level/World HJSON Domain

`uai.world.edit/1` is the persisted Level/World document schema consumed only
through the shared `client.content_graph.import_workspace(...)` ingress. Its
`source` is exactly one of `{"kind":"inline","document":...}`,
`{"kind":"single_file","path":...}`, or
`{"kind":"directory","path":...}`. The common loader reads and verifies a
directory manifest, every declared file, content hash, normalized path, root
boundary, and symlink boundary before dispatching one resolved source to this
provider. There is no `level_apply_folder`, domain file command, or client-side
per-file apply loop. Use
`client.blueprint.component_candidates(...)` for exact component candidates,
`client.content_graph.diagnostics(...)` for target diagnostics, and public
content-graph read methods for revision and independent state. Use
`client.command_search(...)` and `client.command_detail(...)` to inspect live
contracts; `uai-cli` exposes the same discovery path. No task-specific helper,
`world_actor_candidates` alias, or tutorial-owned registry is authoritative.

The T26 provider is a thin adapter over the shared HJSON lifecycle. T13 is the
sole common recursive initialization authority; T26 registers only the World
root and owned/container Actor, Component, and Data Layer initialization
contracts and consumes that seam. It does not implement a second recursive
initializer, Actor factory, component writer, Data Layer owner, persistence
path, readback authority, or cleanup path.

`graph.layout.apply` is not a Level/World HJSON responsibility. T26 contributes
no layout command or layout alias; the unified T62 owner is the only design
authority for persistent graph layout.

Every document carries one explicit three-dimensional space contract: semantic,
write, and read frames are `ue_world`, and unit is `centimeter`. Actor transforms
are World-space. SceneComponent transforms are parent-relative and include an
exact parent component and optional socket. Actor hierarchy uses stable
`managed_id` references. Unsupported types, roles, frames, units, parents, or
stale revisions are rejected before their affected write begins.

Actor and Component creation, managed identity, hierarchy, attachment,
transforms, rollback, and state capture delegate to Level Authoring. Data Layer
creation and membership delegate to the T59 `world-command-owner`; exact native
references are resolved through public readback. Non-HJSON commands such as
`world.data_layer_instance.create`, `world.data_layer_relation.update`, and
`world.wall.create_with_opening` remain with T59 and are not re-registered.

Successful import saves the World and World Partition external Actor packages,
reloads the map, and returns independent `content_graph.*` readback. Test-world
cleanup uses public `asset.delete`; the destructive planning boundary includes
the exact `__ExternalActors__` and `__ExternalObjects__` package set in its
authorization fingerprint and verifies the complete set absent after deletion.
