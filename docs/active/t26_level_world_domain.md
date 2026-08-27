# T26 Level/World HJSON Domain

`uai.world.edit/1` is the persisted Level/World document schema consumed only
through the shared `client.content_graph.import_workspace(...)` ingress. Its
`source` is exactly one of `{"kind":"inline","document":...}`,
`{"kind":"single_file","path":...}`, or
`{"kind":"directory","path":...}`. The common loader reads and verifies a
directory manifest, every declared file, content hash, normalized path, root
boundary, and symlink boundary before dispatching one resolved source to this
provider. There is no `level_apply_folder`, domain file command, or client-side
per-file apply loop. Use `client.world.actor_candidates(...)` for the same
native candidates exposed by the Place Actors UI,
`client.blueprint.component_candidates(...)` for exact Blueprint Component
candidates, `client.content_graph.diagnostics(...)` for target diagnostics, and
public content-graph read methods for revision and independent state. Use
`client.query.command_search(...)` and `client.query.command_detail(...)` to inspect live
contracts; `uai-cli` exposes the same discovery path. No task-specific helper,
legacy `node_catalog_search`, or tutorial-owned registry is authoritative.

`world.actor.candidates` reads `IPlacementModeModule`/`FPlaceableItem`, applies
the native Place Actors category and text-filter semantics, and returns compact
`name` and `id` selectors. `name` may be used only when unique in the selected
native context; `id` preserves the exact category, asset factory, asset override
and Actor class. Richer returned fields may be copied as assertions but are not
required for creation. A native item that relies on a custom drag handler is
still discoverable and is marked unsupported for headless creation instead of
being silently replaced by a class-only path. A category implemented only as an
arbitrary `CustomGenerator` widget and exposing no `FPlaceableItem` entries is
reported as a non-queryable native surface; it is not claimed as headless
creation coverage.

Actor instance identity and placement remain outside the candidate:

```hjson
actors: [
  {
    managed_id: T85NativeCube
    operation: upsert
    candidate: { id: "candidate:..." }
    transform: {
      translation: { components: [0, 0, 50] }
      rotation: { components: [0, 0, 0] }
      scale: { components: [1, 1, 1] }
    }
  }
]
```

Actor `transform` 的精确写入形状必须从同一目标当前返回的
`content_graph.authoring_schema` 读取。Canonical 形态如上；每个分量对象只含长度为
3 的有限数值 `components`。兼容输入还接受 `location:{x,y,z}`、
`rotation:{pitch,yaw,roll}` 和 `scale:{x,y,z}`，服务端会在进入 Level Authoring 前
规范化；`location` 与 `translation` 并存，或同一 rotation/scale 同时提供
`components` 与命名轴，都会在 mutation 前拒绝。裸数组不是 World Actor transform
字段。省略 `transform` 表示保留既有 placement，因此最小创建模板仍只需要
`managed_id + candidate`。这套 World 领域形状不替代通用 UObject property codec 中
`value_kind:transform` 的独立字段合同。

`placement_context.category` is needed only when the query used a category
other than `AllClasses`. Before write, the World provider re-enumerates the same
native context and submits the selected `FPlaceableItem` plus its asset factory
to `UPlacementSubsystem`, the same placement owner used by the current editor
path. It does not reconstruct a class/factory/asset mapping from display text or
replace native placement with the legacy ActorFactory fallback. Existing `type`-based Actor
documents remain a compatibility path, not a native-equivalent candidate path.

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
transforms, rollback, and state capture delegate to Level Authoring. Candidate
Actor creation reuses the shared native Place Actors provider and selected
asset factory through `UPlacementSubsystem`; the World HJSON adapter does not
own a second candidate catalog or placement implementation.
Data Layer
creation and membership delegate to the T59 `world-command-owner`; exact native
references are resolved through public readback. Non-HJSON commands such as
`world.data_layer_instance.create`, `world.data_layer_relation.update`, and
`world.wall.create_with_opening` remain with T59 and are not re-registered.

Successful import saves the World and World Partition external Actor packages,
reloads the map, and returns independent `content_graph.*` readback. Test-world
cleanup uses public `asset.delete`; the destructive planning boundary includes
the exact `__ExternalActors__` and `__ExternalObjects__` package set in its
authorization fingerprint and verifies the complete set absent after deletion.
