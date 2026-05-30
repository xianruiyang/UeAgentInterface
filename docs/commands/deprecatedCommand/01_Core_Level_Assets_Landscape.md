# 废弃指令：Core / Level / Assets / Landscape

> 本文件从 `../01_Core_Level_Assets_Landscape.md` 抽出当前 Level 场景实例 authoring 中已被 `level_content_*_json` 覆盖的旧原子写入命令。它们仍保留兼容，不从路由删除；新建关卡内容、批量修改和托管清理默认应使用 `../23_LevelContent_JSON.md`。

## 判定口径

- 只针对“当前 Level 中持久 Actor / Component 最终态”的 authoring：创建、删除、克隆、普通 transform、folder、tag、Actor 属性、组件属性、TargetPoint probe、NavMeshBoundsVolume 放置。
- 读取、诊断、视口、截图、runtime probe、资产本体编辑、专用几何/导航算法不进入本文件。
- 2026-05-02 已用打开的 UE 编辑器实测：旧原子命令与 Level Content JSON 等价流程均执行成功，并在清理后确认测试 folder 资源数为 0。主要报告：
  - `UeAgentInterfaceCMD/dist/reports/batch_20260502_204332_536349.json`
  - `UeAgentInterfaceCMD/dist/reports/batch_20260502_194147_389747.json`
  - `UeAgentInterfaceCMD/dist/reports/batch_20260502_194250_034514.json`
- 2026-05-02 复核发现原先两个例外是 Level Content adapter 缺陷，而非不可替代的设计边界：`nav_mesh_bounds_volume` 漏创建 Brush Bounds，组件碰撞 profile 漏 `SetCollisionProfileName` 与即时物理刷新。修复后 smoke 覆盖会检查 NavBounds 非零 extent、`nav_bounds_updated=true`，以及 `NoCollision` patch 后 `level_trace_world_ray hit=false`。

## Level Actor CRUD / Probe

| 指令 | 废弃说明 | 新流程 |
| --- | --- | --- |
| `spawn_actor` | **Deprecated for Level Content authoring**：普通 Actor 创建、label、folder、tag、static mesh 和 transform 已由 Level Content resource 表达。 | `level_content_apply_json` 的 `resources[].operation=create/upsert`，使用 `kind` 或 `class_path`。 |
| `destroy_actor` | **Deprecated for Level Content authoring**：单 Actor 删除应进入托管 delete/replace 语义。 | `resources[].operation=delete`、`operations[].type=delete` 或 `level_content_delete_scope`。 |
| `level_duplicate_actor` | **Deprecated for Level Content authoring**：克隆后 patch transform/folder/tag/property 可在同一份 JSON 中完成。 | `operations[].type=clone`，配合 `patch`、`folder_path`、`tags`。 |
| `level_mark_probe` | **Deprecated for Level Content authoring**：单个 TargetPoint probe 可作为普通 resource 创建。 | `resources[]` 中使用 `kind=probe` 或 `kind=target_point`。 |
| `level_generate_probes` | **Deprecated for Level Content authoring**：批量 probe 可由多个 resource 表达并带稳定 `id`。 | 多个 `kind=probe` resource。 |

`level_spawn_wall_with_opening` 不进入废弃：它是带开口墙体的语义生成器，不只是放置一个 Actor。生成后的墙段可以由 Level Content 管理，但拆分算法仍属于专用命令。

兼容说明：`spawn_actor` 仍可用于 bootstrap、探针验证和故障补修。若它运行在显式 `begin_transaction` / `end_transaction` 内，取消事务时会登记并清理本事务中新建的 Actor，`end_transaction(commit=false)` 会报告 `registered_spawned_actor_count` 和 `rolled_back_spawned_actor_count`。

## Transform / Attach / Folder / Tag

| 指令 | 废弃说明 | 新流程 |
| --- | --- | --- |
| `level_set_actor_transform` | **Deprecated for ordinary Level Content authoring**：普通 location/rotation/scale 最终态已由 resource transform 覆盖。带 `collision_aware` 的定点修复仍保留专用用途。 | `resources[].transform` 或 `operation=patch`。 |
| `level_set_actor_location` | **Deprecated for Level Content authoring**。 | `transform.location`。 |
| `level_set_actor_rotation` | **Deprecated for Level Content authoring**。 | `transform.rotation`。 |
| `level_set_actor_scale` | **Deprecated for Level Content authoring**。 | `transform.scale`。 |
| `level_attach_actor` | **Deprecated for Level Content authoring**。 | `resources[].attach_to` 或 `operations[].type=attach`。 |
| `level_detach_actor` | **Deprecated for Level Content authoring**。 | `operations[].type=detach`。 |
| `level_set_actor_folder` | **Deprecated for Level Content authoring**。 | `resources[].folder_path` 或 `operations[].type=set_folder`。 |
| `level_add_actor_tag` | **Deprecated for Level Content authoring**。 | `resources[].tags` 或 `operations[].type=add_tag/remove_tag`。 |

## 属性写入

| 指令 | 废弃说明 | 新流程 |
| --- | --- | --- |
| `actor_set_property` | **Deprecated for Level Content authoring**：Actor 反射属性 patch 可跟随资源声明。 | `resources[].properties`。 |
| `level_set_actor_property` | **Deprecated for Level Content authoring**：同 `actor_set_property`。 | `resources[].properties`。 |
| `component_set_property` | **Deprecated for Level Content authoring**：组件反射属性 patch 可跟随资源声明；`BodyInstance.* / Collision* / CanCharacterStepUpOn` 已在 Level Content 中补齐物理状态刷新，`BodyInstance.CollisionProfileName` 会调用 `SetCollisionProfileName`。 | `resources[].properties` 中使用 `ComponentName.PropertyName`，或 `components[].properties`。 |
| `level_set_component_property` | **Deprecated for Level Content authoring**：同 `component_set_property`。 | 同上。 |

## Navigation Bounds 放置

| 指令 | 废弃说明 | 新流程 |
| --- | --- | --- |
| `navmesh_spawn_bounds_volume` | **Deprecated for Level Content authoring**：创建 `ANavMeshBoundsVolume`、Brush Bounds 和 `OnNavigationBoundsUpdated` 已由 Level Content `nav_mesh_bounds_volume` adapter 覆盖。 | `resources[]` 中使用 `kind=nav_mesh_bounds_volume`，提供 `bounds.center/extent` 或 `bounds.min/max`，必要时设置 `update_navigation_bounds`。 |

## 托管清理

| 指令 | 废弃说明 | 新流程 |
| --- | --- | --- |
| `level_destroy_folder_actors` | **Deprecated for managed Level Content cleanup**：托管 scope 删除应带 managed/protected 规则。 | `level_content_delete_scope` 或 `delete_policy=replace_scope_managed`。 |
| `level_cleanup_empty_folders` | **Deprecated for managed Level Content cleanup**：Level Content 流程内的清理应由 repair/task 统一记录。 | `tasks[].type=cleanup_empty_folders` 或 `level_content_repair_json`。 |

## 明确保留的非废弃命令

| 指令 / 指令族 | 保留原因 |
| --- | --- |
| `list_actors`、`level_get_actor_transform`、`level_get_actor_property`、`actor_list_components`、`level_list_actor_components`、`level_get_component_property` | 只读诊断命令；Level Content query/snapshot 可批量替代工作流，但这些原子读命令仍适合定位问题。 |
| `navmesh_build`、`navmesh_project_point`、`navmesh_find_path`、`navigation_*` | 导航系统 build/query/diagnostic，不是普通 Actor CRUD。 |
| `level_trace_world_ray`、`level_sweep_capsule*`、`level_check_overlaps`、`level_validate_connectivity` | 空间诊断和验证工具；Level Content validation 可聚合调用，但不能替代原子调试入口。 |
| `level_snap_to_surface`、`level_align_actor_*`、`mesh_get_*` | 几何求解和对齐算法，不只是写最终 transform。 |
| `level_set_skeletal_mesh_morph_target` | Editor/runtime 展示控制和 pulse ticker，不是持久 Level JSON 最终态。 |
| `landscape_*`、`modeling_*` | 地形或几何资产/工具语义，不属于通用 Level Actor CRUD。 |
