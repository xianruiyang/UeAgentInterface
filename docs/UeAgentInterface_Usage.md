# UeAgentInterface 调用文档（UTF-8）

本文档位于插件目录：`Plugins/UeAgentInterface/docs/UeAgentInterface_Usage.md`。
每次新增或修改命令时，请同步更新本文档。

## 0. 详细指令文档（按模块）

如果你需要“每个指令的作用 + 参数 + 典型用法”，请直接查看：

- `Plugins/UeAgentInterface/docs/commands/README.md`
- `Plugins/UeAgentInterface/docs/UeAgentInterface_Status.md`（当前已完成能力与边界）
- `Plugins/UeAgentInterface/docs/Workflow_ExecBatch_Practice.md`（批量执行与高效调用流程）
- `Plugins/UeAgentInterface/docs/PLAN_Packaged_Python_CLI.md`（外置打包 CLI 方案草案）
- `UeAgentInterfaceCMD/docs/USAGE.md`（外置命令行工具的实际使用说明）
- `Plugins/UeAgentInterface/docs/commands/01_Core_Level_Assets_Landscape.md`
- `Plugins/UeAgentInterface/docs/commands/02_Blueprint.md`
- `Plugins/UeAgentInterface/docs/commands/03_UMG.md`
- `Plugins/UeAgentInterface/docs/commands/04_StaticMesh_EnhancedInput.md`
- `Plugins/UeAgentInterface/docs/commands/05_Material.md`
- `Plugins/UeAgentInterface/docs/commands/06_Sequence.md`
- `Plugins/UeAgentInterface/docs/commands/07_Niagara_System.md`
- `Plugins/UeAgentInterface/docs/commands/08_Niagara_Emitter.md`
- `Plugins/UeAgentInterface/docs/commands/09_Niagara_StageGraph.md`
- `Plugins/UeAgentInterface/docs/commands/15_Niagara_FolderFormat.md`
- `Plugins/UeAgentInterface/docs/commands/16_SkeletalMesh_FolderFormat.md`
- `Plugins/UeAgentInterface/docs/commands/17_ControlRig_FolderFormat.md`
- `Plugins/UeAgentInterface/docs/commands/18_Deformer_MLDeformer_GeometryCache.md`
- `Plugins/UeAgentInterface/docs/commands/10_Modeling.md`

### 0.1 外置 CLI 推荐用法（UeAgentInterfaceCMD）

为减少重复输入和降低误操作，推荐通过 `UeAgentInterfaceCMD/dist/uai-cli.exe` 调用插件，而不是手写 HTTP。

建议顺序：

1. `doctor` 先检查服务连通性；
2. `run`（`plan + vars`）或 `batch`（JSON 文件）执行任务；
3. 查看 `reports/*.json` 定位失败步骤与失败命令；
4. 需要短命名时用 `namegen` 生成资产名（避免 Blueprint 名过长难检索）。

Niagara 相关批次默认启用护栏（自动 preflight + 编译日志 + emitter 列表校验）。  
如需关闭（不推荐），才使用 CLI 参数 `--no-niagara-guard`。

### 0.2 当前文档边界

本文档描述的是**当前已经完成并可用的接口**。  
对仍处于边界状态或已知不稳定的能力，会明确标注，不会写成“完全支持”。

### 0.3 资产编辑主工作流

当前文档口径统一如下：

- 资产编辑默认优先使用 `JSON / 结构化 JSON`
- 能被 JSON / 结构化 JSON 覆盖的写入型原子命令统一 deprecated for authoring，不再作为资产 authoring 的首选入口
- 对 Niagara，完整效果制作必须走文件夹式结构化 JSON：创建最小骨架 -> 导出 -> 写基础 module -> apply -> 再导出补全属性 -> 修改完整参数 -> 再 apply -> 检查 apply 返回的 Stack issue / compile log / runtime probe

推荐优先级：

1. 小型、浅层、字段不多的资产：
   - 优先单文件 JSON
   - 典型命令：
     - `asset_export_property_json / asset_apply_property_json`
     - `curve_export_json / curve_apply_json`
     - `enhanced_input_export_action_json / enhanced_input_apply_action_json`
     - `enhanced_input_export_mapping_context_json / enhanced_input_apply_mapping_context_json`
2. 图类、树类、结构较复杂的资产：
   - 优先文件夹式结构化 JSON
   - 典型命令：
     - `blueprint_export_folder / blueprint_apply_folder`
     - `umg_export_folder / umg_apply_folder`
     - `anim_blueprint_export_folder / anim_blueprint_apply_folder`
     - `material_export_folder / material_apply_folder`
     - `material_instance_export_folder / material_instance_apply_folder`
     - `material_function_export_folder / material_function_apply_folder`
     - `niagara_export_folder / niagara_apply_folder`
     - `niagara_emitter_export_folder / niagara_emitter_apply_folder`
     - `niagara_script_export_folder / niagara_script_apply_folder`
     - `static_mesh_export_folder / static_mesh_apply_folder`
     - `skeletal_mesh_export_folder / skeletal_mesh_apply_folder`
     - `deformer_graph_export_folder / deformer_graph_apply_folder`
3. 原子命令：
   - 主要用于以下场景：
     - bootstrap 或补创建某个最小局部结构
     - 读取 live 信息、调试、探针验证
     - 结构化 schema 边界字段
     - 回写失败后的定点修补
   - 当同一字段或结构已经能被单文件 JSON / 文件夹式结构化 JSON 表达时，对应写入型原子命令统一标记为 `Deprecated for authoring`

一句话约束：

- 能走 JSON / 结构化 JSON 的资产编辑，不要退回逐条原子命令手搓完整 authoring。
- 被 JSON / 结构化 JSON 覆盖的写入型原子命令保留兼容，但统一视为 deprecated for authoring；只用于 bootstrap、诊断、迁移、schema 边界和紧急局部修补。
- 读取、创建最小资产、打开编辑器、编译、截图、导出、应用、dirty 处理和诊断命令不属于这类废弃写入命令。
- 暂无 JSON / 结构化 JSON profile 的领域不强行标废弃，例如 Modeling、Level Actor 放置、NavMesh、普通视口控制等。IK Rig / IK Retargeter / Skeleton / SkeletalMesh / Control Rig / StaticMesh / Deformer Graph 已进入 JSON 或文件夹式结构化 JSON 主流程。

废弃命令明细已抽到 `commands/deprecatedCommand/`，覆盖面按主流程分组：

- `asset_apply_property_json` 覆盖的纯属性写入：`static_mesh_set_property`、`anim_sequence_set_settings`、以及其他能被导出的 `properties[]` 准确表达的资产属性 setter。
- `blueprint_apply_folder` 覆盖的 Actor Blueprint 写入：组件树、成员、图节点/连线、pin 默认值、CDO 默认值、reparent。
- `umg_apply_folder` 覆盖的 WidgetBlueprint 写入：控件树、Widget/Slot 属性、属性绑定、常见 UMG 动画结构。
- `enhanced_input_apply_*_json` 覆盖的 EnhancedInput 写入：Action / MappingContext 属性、mapping 增删和清空。
- `material*_apply_folder` 覆盖的材质写入：Material 根属性、Material Instance parent/parameter override、Material Function 属性、表达式节点与连线。
- `sequence_apply_folder` 覆盖的 Level Sequence 写入：播放设置、binding、track、key、section、master track、outliner folder。
- `montage_apply_json` 覆盖的 Montage 写入：blend/sync、preview、slot track、segment、section、notify、Skeleton slot/group。
- `anim_blueprint_apply_folder` 覆盖的 AnimBlueprint 写入：成员、逻辑图、Anim Layer、Layer Interface、State Machine、State/Conduit/Alias/Transition、预览配置。
- `niagara*_apply_folder` 覆盖的 Niagara 写入：System、Emitter、Script、Renderer、Event、Parameter、Stage、Module、Node、ModuleInput。
- `static_mesh_apply_folder` 覆盖的 StaticMesh authoring 写入：材质、socket、simple collision、lightmap、Nanite 安全字段。
- `skeletal_mesh_apply_folder` 覆盖的 SkeletalMesh authoring 写入：材质、mesh-only socket、physics、post process AnimBP、Morph 删除；Skin Weight Profile 导入/删除/预览和 Morph 预览使用显式动作命令，不隐式混进 folder apply。
- `deformer_graph_apply_folder` 与 Deformer/ML 单文件 JSON 覆盖的写入：显式 `apply=true` 的 UObject 属性项。

### 0.4 通用编辑方法论

对“属性很多、类型很多、细节很难一次写全”的对象，当前推荐统一采用：

`bootstrap -> export -> refine -> apply`

具体步骤：

1. `bootstrap`
   - 先只创建最小骨架
   - 只写必需结构，不强行一次写全全部属性
2. `export`
   - 让 UE 先把真实对象建出来
   - 再导出 JSON / 结构化 JSON，拿到 UE 当前真实认可的字段和值格式
3. `refine`
   - 在导出的 JSON 基础上补全高价值属性
   - 用导出的真实字段做模板，而不是靠记忆硬猜属性名和 `value_text`
4. `apply`
   - 再次回写并验证

这套方法适合：

- Blueprint `components/tree.json`
- UMG `widget_tree/tree.json`
- Material expression / instance parameter 真源
- AnimBlueprint 的结构层与图层
- 各类小型资产的单文件 JSON 属性工作流

选择格式时按对象复杂度区分：

- 小型、浅层对象：
  - 优先单文件 JSON
- 大型、树状、图状对象：
  - 优先文件夹式结构化 JSON

这条方法论的目标不是减少步骤，而是减少“第一次就把属性写错”的概率，并把后续编辑建立在 UE 已经吐出的真实模板上。

### 0.5 JSON / 结构化 JSON 失败返回契约

- 单文件 JSON：
  - `json_file` 缺失、文件不存在、文件读取失败、JSON 语法解析失败都会直接失败返回，错误码为 `missing_json_file`、`json_file_not_found`、`load_json_file_failed` 或 `json_parse_failed`。
- `asset_apply_property_json` 的 `properties[]` 条目不是对象、缺 `property_name`、缺 `value_text`、字段拼写错误或 `ImportText` 失败时，都会在 `property_results[]` 中保留失败项；JSON 结构问题会写入对应结果的 `json_issues[]`。
- 文件夹式结构化 JSON：
  - 必需 JSON 文件解析失败直接失败返回。
  - 可选 JSON 文件仅在“不存在”时跳过；只要文件存在但读取失败或 JSON 语法解析失败，就会失败返回，并把具体文件路径拼进错误字符串。
  - 数组条目不是对象、缺关键字段、字段无法解析但不需要中断整次 apply 的情况，会进入 `warnings[]` / `warning_count`。
  - Niagara 仍额外支持 `strict=true`，可把 `warnings[]` 升级为失败；其他 folder workflow 默认以“硬解析失败直接失败、可恢复结构问题进入 warnings”的口径执行。

## 1. 启动与鉴权

- 菜单启动：`Window -> UeAgentInterface -> Start UeAgentInterface Server`
- 菜单停止：`Window -> UeAgentInterface -> Stop UeAgentInterface Server`
- 连接信息：`Window -> UeAgentInterface -> Copy Connection Info`
- 默认地址：`http://127.0.0.1:17777`
- 请求头：`X-UeAgentInterface-Token: <token>`

## 2. 基础 HTTP 接口

- `GET /api/ping`
- `GET /api/status`
- `POST /api/exec`

`/api/exec` 请求体：

```json
{
  "request_id": "req-001",
  "command": "list_actors",
  "params": {}
}
```

`/api/exec` 批量执行（遇错中断）：

```json
{
  "request_id": "req-batch-001",
  "command": "exec_batch",
  "params": {
    "stop_on_error": true,
    "commands": [
      {
        "request_id": "step-1",
        "command": "list_actors",
        "params": { "limit": 20 }
      },
      {
        "request_id": "step-2",
        "command": "spawn_actor",
        "params": {
          "class_path": "/Script/Engine.StaticMeshActor",
          "location": { "x": 0, "y": 0, "z": 100 }
        }
      }
    ]
  }
}
```

批量返回要点：
- `ok=false` 时会中断后续步骤（默认 `stop_on_error=true`）
- `data.failed_index`：中断步骤索引（从 0 开始）
- `data.failed_error`：中断错误
- `data.results`：包含已执行步骤的完整结果（成功步骤也会返回）

## 3. 参数约定

- 向量：`{"x":0,"y":0,"z":0}`
- 旋转：`{"pitch":0,"yaw":0,"roll":0}`
- 复杂属性写入：`value_text`（UE ImportText 格式）
- 任何走 `value_text` / 单文件 JSON / 文件夹式结构化 JSON 的属性回写，都应返回或记录解析与写回观测信息：`requested_value_text`、`applied_value_text`、`property_value_read_back`、`property_import_status`、`property_import_verified`、`property_import_error`、`value_text_exact_match`、`value_text_changed_after_import`、`cpp_type`。批量/文件夹式 apply 会把缺字段、字段类型不对、属性不存在、`ImportText` 失败、写后读回不一致写入 `warnings`、`json_issues[]` 或 `property_results[]`。
- `value_text_changed_after_import=true` 不是自动失败；UE 可能会规范化导入文本。但对向量、颜色、枚举和对象引用，它是发现“命令成功但实际值没有按请求落下”的关键返回字段。

## 4. 指令总览

### 4.1 世界 / 关卡 / 视口 / 事务

- `get_world_state`
- `list_actors`
- `level_get_selection`
- `level_set_selection`
- `actor_list_components`
- `level_list_actor_components`
- `level_get_actor_property`
- `level_get_component_property`
- `level_get_actor_transform`
- `actor_set_property`
- `level_set_actor_property`
- `component_set_property`
- `level_set_component_property`
- `level_set_actor_transform`
- `level_set_actor_location`
- `level_set_actor_rotation`
- `level_set_actor_scale`
- `spawn_actor`
- `level_spawn_wall_with_opening`
- `destroy_actor`
- `level_mark_probe`
- `level_generate_probes`
- `level_duplicate_actor`
- `level_attach_actor`
- `level_detach_actor`
- `level_set_actor_folder`
- `level_destroy_folder_actors`
- `level_cleanup_empty_folders`
- `level_add_actor_tag`
- `navmesh_build`
- `navmesh_project_point`
- `navmesh_find_path`
- `navmesh_spawn_bounds_volume`
- `level_validate_connectivity`
- `viewport_get_camera`
- `viewport_set_camera`
- `viewport_set_realtime`
- `viewport_set_game_view`
- `viewport_focus_actor`
- `viewport_focus_actor_safe`
- `viewport_frame_actor`
- `viewport_frame_selection`
- `viewport_frame_actors`
- `viewport_frame_folder`
- `viewport_deproject_screen_to_world`
- `viewport_trace_screen_point`
- `level_trace_world_ray`
- `level_sweep_capsule`
- `level_sweep_capsule_path`
- `level_check_overlaps`
- `viewport_pick_actor_at_screen`
- `viewport_select_actor_at_screen`
- `level_get_nearby_actor_obbs`
- `screenshot_viewport`
- `screenshot_viewport_buffer`
- `mesh_get_closest_vertex`
- `mesh_get_vertex_world_position`
- `level_align_actor_vertex_to_vertex`
- `level_align_actor_by_bounds`
- `level_align_face_to_face`
- `save_current_level`
- `begin_transaction`
- `end_transaction`
- `undo`
- `redo`
- `exec_batch`

`viewport_set_realtime` 参数：
- `realtime`（必填，`true/false`）
- `store_current_value`（可选，默认 `false`）

`viewport_set_game_view` 参数：
- `game_view`（必填，`true/false`）

Level / viewport / 顶点对齐补充：
- `level_get_selection`：返回当前 Outliner 选中的 Actor 列表。
- `level_set_selection`：设置选择集；参数 `actor_ids`、`mode`（`replace/add/remove`）。
- `level_list_actor_components`：`actor_list_components` 的同义命令。
- `spawn_actor`：生成 Actor；参数必填 `class_path`，可选 `label/location/rotation/scale/static_mesh/folder_path/tags[]`；并支持 `snap_to_ground/ground_offset/require_ground_hit` 等“贴地生成”参数。
- `level_get_actor_property`：读取关卡 Actor 属性；参数 `id`、`property_name`。
- `level_get_component_property`：读取关卡 Actor 某组件属性；参数 `id`、`component`（或 `component_id`）、`property_name`。
- `level_get_actor_transform`：读取 Actor 当前实例变换；参数 `id`，返回 `location`、`rotation`、`scale` 与整合后的 `transform`。
- `level_set_actor_property` / `level_set_component_property`：分别是 `actor_set_property` / `component_set_property` 的同义命令。
- `level_set_actor_transform`：一次性写入 `location` / `rotation` / `scale`；参数 `id`、可选 `location`、`rotation`、`scale`；可选 `collision_aware=true` + `collision{...}` 做“放置前 Overlap 检测 + 多候选偏移”，避免穿插/卡住/缝隙。
- `level_set_actor_location` / `level_set_actor_rotation` / `level_set_actor_scale`：按单个维度写实例变换；参数 `id` + 对应值。
- `level_duplicate_actor`：复制指定 Actor；参数 `id`、可选 `offset`。
- `level_attach_actor`：把 `child_id` 附加到 `parent_id`，保持世界变换。
- `level_detach_actor`：分离指定 Actor，保持世界变换。
- `level_set_actor_folder`：设置 Actor 在 Outliner 里的文件夹路径；参数 `id`、`folder_path`。
- `level_destroy_folder_actors`：按 Outliner `folder_path` 批量删除 Actor；参数 `folder_path`、可选 `include_child_folders`（默认 `true`）。当 folder 下无 Actor 时也会返回成功，适合做幂等清理。
- `level_cleanup_empty_folders`：清理当前世界里未被任何 Actor 使用的 Outliner folder；参数可选 `folder_path_prefix`（只清某个前缀下的空 folder）、`dry_run`（只预览不删除）。
- `level_add_actor_tag`：给 Actor 增加 tag；参数 `id`、`tag`。
- `level_mark_probe`：创建一个 `TargetPoint` 作为验收探针；参数 `location`，可选 `label/rotation/folder_path/tags`。
- `level_generate_probes`：批量创建 `TargetPoint` 探针；参数二选一：`points[]`（vector 列表）或 `probes[]`（对象列表，含 `location`），可选 `label_prefix/rotation/folder_path/tags`。
- `level_spawn_wall_with_opening`：生成带开口的墙（自动拆分为最多 4 段 StaticMeshActor）；参数 `plane.center/normal/up`、`wall_size.thickness_cm/width_cm/height_cm`、`opening_center.right_cm/up_cm`、`opening_size.width_cm/height_cm`；可选 `opening_padding_cm`（门洞净空）、`min_segment_size_cm`、`clamp_opening`、`label_prefix`、`folder_path`、`class_path`（默认 StaticMeshActor）、`static_mesh`（默认 Cube）、`epsilon_cm`。
- `navmesh_build`：在 Editor World 触发导航构建/刷新；参数可选 `wait_for_finish`、`timeout_seconds`。注意：需要关卡内存在有效 `NavMeshBoundsVolume` 才会产出可查询 Nav 数据。
- `navmesh_project_point`：把某个世界点投影到 NavMesh；参数 `point`（或 `location`），可选 `project_query_extent`；返回 `projected` 与 `projected_point`（若成功）。适合在查路前先确认投影是否落到了“期望层面”。
- `navmesh_find_path`：基于 NavMesh 查询两点是否连通并返回路径点；参数 `start/end`，可选 `allow_partial`、`project_to_nav`、`project_query_extent`、`allow_projection_failure`（投影失败不作为硬错误，用于诊断）。
- `navmesh_spawn_bounds_volume`：创建 `NavMeshBoundsVolume` 并生成有效 Brush（避免直接 `spawn_actor` 导致 bounds 为 0、NavMesh 无法投影/查路）；参数必填 `bounds.center/extent`，可选 `rotation/label/folder_path/update_navigation_bounds`。
- `level_validate_connectivity`：对一组点/探针做“按对/按序”的连通性验收；参数二选一：`points[]` 或 `probe_actor_ids[]`；可选 `pairs[]`（显式指定 `from_index/to_index`，并可选 `edge_type`，默认 `walk`；`edge_type!=walk` 仅做“端点投影/距离阈值”校验，不验证设备语义）；可选 `include_path_points/max_path_points`；可选 `graph_root_index` 输出 `graph{reachable/sccs/...}`；返回 `all_connected` 与每对结果。
- `viewport_frame_selection`：聚焦当前选择集；参数 `instant`（可选，默认 `true`）。
- `viewport_focus_actor_safe`：更稳的聚焦：默认 `collision_aware=true` + `look_at=true` + `auto_fallback=true`，内部走 `viewport_frame_actor` 的逻辑，减少室内取景卡墙/朝向不对。
- `viewport_frame_actor`：按单个 Actor bounds 重新计算取景点；默认只平移 `location`，`look_at=true` 时会同步调整 `rotation`（`roll=0`）。支持 `collision_aware` 以及 `auto_fallback/fallback_step_cm/fallback_offsets_cm`（室内防卡墙多候选）。仅支持透视视口。
- `viewport_frame_actors`：聚焦指定 Actor 列表；参数 `actor_ids`、可选 `instant`；同样支持可选 `collision_aware` 与 `auto_fallback/fallback_step_cm/fallback_offsets_cm`。
- `viewport_frame_folder`：按 Outliner `folder_path` 聚合 Actor bounds 并重新计算取景点；默认只平移 `location`，`look_at=true` 时会同步调整 `rotation`（`roll=0`）。支持 `collision_aware` 与 `auto_fallback/fallback_step_cm/fallback_offsets_cm`。仅支持透视视口。
- `screenshot_viewport`：抓取当前视口截图；参数可选 `format`（`png/jpg/webp`）、`quality`（1~100）、`max_size`（最大边长等比缩放）；返回 `path` 与 artifact。
- `screenshot_viewport_buffer`：抓取视口 buffer 截图（默认 `SceneDepth`）；参数可选 `buffer`（`SceneDepth/DeviceDepth/WorldNormal/BaseColor/...`）与同 `screenshot_viewport` 的输出格式参数；对 `SceneDepth` 可选 `depth_mode=auto_percentile/fixed`、`depth_near_cm/depth_far_cm`、`depth_auto_pct_low/high`，用于避免“纯黑/纯白深度图”。
- `viewport_deproject_screen_to_world`：屏幕点反投影；参数 `screen_x`、`screen_y`，返回 `world_origin` 与 `world_direction`。
- `viewport_trace_screen_point`：从屏幕点发起 Trace；参数 `screen_x`、`screen_y`、可选 `trace_distance`、`trace_channel`、`trace_complex`，返回命中位置、法线、Actor、Component 等。
- `level_trace_world_ray`：不依赖 viewport 的世界射线 Trace；单条参数 `start`、`direction`、可选 `trace_distance`/`trace_channel`/`trace_complex`/`ignore_actor_ids`，以及可选 `ignore_folder_path_prefix/ignore_tags/ignore_class_substrings`；也支持批量模式 `rays[]`（可选 `max_items/continue_on_error`）。
- `level_sweep_capsule`：用胶囊体做 Sweep 通行性检测；单条参数 `start/end`、`radius_cm`、`half_height_cm`，可选 `trace_channel`/`trace_complex`/`find_initial_overlaps`/`ignore_actor_ids`，以及可选 `ignore_folder_path_prefix/ignore_tags/ignore_class_substrings`；可选 `return_penetration_depth=true` 在 `start_penetrating=true` 时返回 `penetration_depth_cm`；也支持批量模式 `sweeps[]`（可选 `stop_on_blocking_hit/max_items/continue_on_error`）。
- `level_sweep_capsule_path`：沿折线采样做多段胶囊 Sweep（路径净空）；参数 `points[]`（默认脚底点，`points_mode=feet/center`）、`radius_cm`、`half_height_cm`，可选 `step_cm`、`snap_to_floor`、`floor_clearance_cm`、`ignore_walkable_floor_hits`、`max_walkable_slope_deg`、`stop_on_blocking_hit`、以及 `include_samples/include_segments` 便于调试；可选 `return_penetration_depth=true` 返回 `first_penetration_depth_cm` 与 `segments[].penetration_depth_cm`。
- `level_check_overlaps`：Overlap 检测（发现互相穿插/卡住）；单条参数 `shape`=`box/sphere/capsule`、`center`，以及形状参数（`box_extent` 或 `radius_cm/half_height_cm`），可选 `rotation`、`trace_channel`、`trace_complex`、`limit`、`ignore_actor_ids`，以及可选 `ignore_folder_path_prefix/ignore_tags/ignore_class_substrings`；也支持批量模式 `checks[]`（可选 `stop_on_overlap/max_items/continue_on_error`）。
- `level_snap_to_surface`：把某个 Actor 沿射线吸附到命中面；参数 `id`，可选 `start/direction/trace_distance/trace_channel/trace_complex/offset_cm/offset_mode/align_rotation/ignore_actor_ids`；返回 `snapped` 与命中信息。
- `viewport_pick_actor_at_screen`：从屏幕点发起 Trace 并返回命中 Actor；参数 `screen_x`、`screen_y`、可选 `trace_distance`、`trace_channel`、`trace_complex`、`ignore_actor_ids`。若 `allow_no_hit=true`，未命中时也会返回成功并给出 `hit=false`（便于脚本循环）。
- `viewport_select_actor_at_screen`：从屏幕点拾取并同步设置当前编辑器选择集；参数 `screen_x`、`screen_y`、可选 `selection_mode=replace/add/remove`、`trace_distance`、`trace_channel`、`trace_complex`、`ignore_actor_ids`。若 `allow_no_hit=true`，未命中时不会修改选择集，只返回当前选择集。
- `level_get_nearby_actor_obbs`：读取某个中心点附近 Actor 的 OBB 信息；参数 `radius`，中心二选一：`id`（从 Actor bounds center 取）或 `center`（显式世界坐标）；可选 `include_self`（仅 `id` 模式有效），以及可选过滤 `folder_path_prefix/ignore_folder_path_prefix/accept_tags/ignore_tags/accept_class_substrings/ignore_class_substrings/limit`。
- `mesh_get_closest_vertex`：在 `UStaticMeshComponent` 上查最近顶点；参数 `id`、可选 `component`、`world_point`。
- `mesh_get_vertex_world_position`：按顶点索引读取世界坐标；参数 `id`、可选 `component`、`vertex_index`。
- `level_align_actor_vertex_to_vertex`：把源 Actor 的一个顶点对齐到目标 Actor 的一个顶点，只修改源 Actor 的实例变换；参数：
  - `source_actor_id`、`target_actor_id`
  - 可选 `source_component`、`target_component`
  - 源顶点二选一：`source_vertex_index` 或 `source_world_point`
  - 目标顶点二选一：`target_vertex_index` 或 `target_world_point`
- `level_align_actor_by_bounds`：按包围盒锚点对齐源 Actor 到目标 Actor；参数：
  - `source_actor_id`、`target_actor_id`
  - `axis`=`x/y/z`
  - `source_anchor` / `target_anchor`=`min/center/max`
  - 可选 `offset`
- `level_align_face_to_face`：对齐两个 Actor 的“某个面”使其贴合；参数 `source_actor_id/target_actor_id`，可选 `axis`，以及 `source_face/target_face`（`min/max/center` 或 `+x/-x/+y/-y/+z/-z`），可选 `offset_cm`。

### 4.2 通用资产编辑

- `editor_get_open_assets`
- `open_asset_editor`
- `save_asset`
- `asset_duplicate`
- `asset_import_texture`
- `asset_import_fbx_skeletal_mesh`
- `asset_import_fbx_animation`
- `asset_export_property_json`
- `asset_apply_property_json`
- `curve_export_json`
- `curve_apply_json`
- `editor_list_dirty_resources`
- `editor_resolve_dirty_resources`
- `editor_close`
- `editor_prepare_exit`

### 4.3 Landscape

- `landscape_create`
- `landscape_raise_circle`

### 4.4 Blueprint

- `blueprint_create`
- `blueprint_compile`
- `blueprint_get_compile_log`
- `blueprint_get_info`
- `blueprint_list_graphs`
- `blueprint_export_folder`
- `blueprint_apply_folder`
- `blueprint_inspect_components`
- `blueprint_inspect_nodes`
- `blueprint_graph_get_view`
- `blueprint_graph_set_view`
- `blueprint_viewport_get_camera`
- `blueprint_viewport_set_camera`
- `blueprint_screenshot`

Blueprint 视图与截图补充：
- `blueprint_graph_get_view` / `blueprint_graph_set_view`：读取/设置指定图（默认 `EventGraph`）的平移和缩放。
- `blueprint_viewport_get_camera` / `blueprint_viewport_set_camera`：读取/设置 Blueprint 预览视口相机（`location`/`rotation`/`fov`）。
- `blueprint_screenshot`：
  - `target` 支持 `viewport` / `graph` / `event_graph` / `window`
  - `target=viewport` 优先截 Blueprint 预览区内部真实 `SViewport` 区域，而不是外层编辑器容器
  - 当预览视口相机仍处于未初始化状态（典型特征：位置/旋转全 0）时，会先自动聚焦到预览 Actor 包围盒，再截图
  - 返回 `capture_mode`，用于区分截图来源：`viewport_window_crop`、`viewport_readpixels_fallback`、`window_fallback`、`slate_widget`
  - 详细调试信息会写入 `Saved/Logs/ueagentinterface_%COMPUTERNAME%.log`
- `blueprint_export_folder` / `blueprint_apply_folder`：
  - 当前这是 `Actor Blueprint` 的主编辑路径
  - 当前只覆盖 `Actor Blueprint`
  - 固定导出根目录：`Saved/UeAssetFolders/ActorBlueprint`
  - 单个资产目录按 `asset_path` 自动展开，例如 `/Game/Foo/BP_Bar` -> `Saved/UeAssetFolders/ActorBlueprint/Game/Foo/BP_Bar`
  - `apply_folder` 当前对支持的图采用“保留内建入口节点，重建其余节点”的方式
  - `graphs/*.json` 当前稳定支持 `event / custom_event / call_function / variable_node / component_bound_event / node_by_class`
  - `function_graph / macro_graph` 的边定义可使用保留节点 ID：`__entry__`、`__result__`
  - 可进入该 folder workflow 的组件、成员、图节点/连线、pin 默认值、CDO 默认值和 reparent 原子写入命令明细见 `commands/deprecatedCommand/02_Blueprint.md`

### 4.5 UMG（WidgetBlueprint）

- `umg_create_widget_blueprint`
- `umg_compile`
- `umg_get_compile_log`
- `umg_get_info`
- `umg_export_folder`
  - 关键参数：`asset_path`
  - 当前这是 `WidgetBlueprint` 的主编辑路径
  - 固定导出根目录：`Saved/UeAssetFolders/WidgetBlueprint`
  - 第一版导出结构：
    - `asset.json`
    - `settings/widget_blueprint.json`
    - `members/variables.json`
    - `widget_tree/tree.json`
    - `bindings/property_bindings.json`
    - `animations/animations.json`
    - `graphs/*.json`
    - `validation/checks.json`
- `umg_apply_folder`
  - 关键参数：`asset_path`
  - 从 `Saved/UeAssetFolders/WidgetBlueprint/<asset_path>` 读取结构化目录并回写
  - 当前稳定面：
    - Blueprint 变量
    - WidgetTree 重建
    - 常见 Widget / Slot 属性
    - `property_variable` / `function` 两类绑定
    - `RenderOpacity / ColorAndOpacity / BackgroundColor / RenderTransform`
    - 通用 `float_property / color_property`
  - 当前控件树采用“优先复用匹配根控件、重建其余子树”的 apply，不是节点级 diff
  - UMG 废弃写入命令明细见 `commands/deprecatedCommand/03_UMG.md`，默认只做初始化、探针验证和局部补修

### 4.12 Animation Blueprint

- `anim_blueprint_create`
  - 关键参数：`asset_path`
  - 可选：`parent_class`、`target_skeleton`、`preview_skeletal_mesh`、`template`、`compile_after_create`、`open_editor`、`save_after_create`
- `anim_blueprint_create_layer_interface`
  - 关键参数：`asset_path`
  - 可选：`compile_after_create`、`open_editor`、`save_after_create`
- `anim_blueprint_open_editor`
  - 关键参数：`asset_path`
- `anim_blueprint_compile`
  - 关键参数：`asset_path`
  - 可选：`include_messages`、`severity_filter`、`max_messages`、`save_after_compile`
- `anim_blueprint_get_compile_log`
  - 关键参数：`asset_path`
  - 可选：`severity_filter`、`max_messages`、`save_after_compile`
- `anim_blueprint_get_info`
  - 关键参数：`asset_path`
  - 返回：`target_skeleton`、`preview_skeletal_mesh`、`preview_animation_blueprint`、`preview_application_method`、`preview_animation_blueprint_tag`、图统计与支持能力
- `anim_blueprint_export_folder`
  - 关键参数：`asset_path`
  - 当前这是 `AnimBlueprint` 的主编辑路径
  - 可选：`clean_output_dir`、`include_validation`
  - 固定导出根目录：`Saved/UeAssetFolders/AnimBlueprint`
- `anim_blueprint_apply_folder`
  - 关键参数：`asset_path`
  - 可选：`create_if_missing`、`compile_after_apply`、`save_after_apply`
  - 当前按 `settings / members / layer_interfaces / anim_layers / state_machines / graphs` 顺序回写
  - AnimBlueprint 废弃写入命令明细见 `commands/deprecatedCommand/11_AnimBlueprint.md`，默认只做初始化、探针验证和局部补修
- `anim_blueprint_list_graphs`
  - 关键参数：`asset_path`
  - 返回：图数组，字段含 `graph_type`、`is_anim_graph`、`graph_kind`
- `anim_blueprint_list_state_machines`
  - 关键参数：`asset_path`
  - 返回：状态机数组，字段含 `state_machine_name`、`owner_anim_graph_name`、`entry_connected_state_name`、`state_count`、`transition_count`、`state_nodes[]`、`transition_nodes[]`
- `anim_blueprint_list_anim_layers`
  - 关键参数：`asset_path`
  - 返回：Anim Layer 数组，合并编辑图与编译函数视图，字段含 `layer_name`、`has_graph`、`compiled_function_found`、`implemented_interface`
- `anim_blueprint_list_layer_interfaces`
  - 关键参数：`asset_path`
- `anim_blueprint_inspect_nodes`
  - 关键参数：`asset_path`、`graph_name`
  - 可选：`limit_per_graph`、`include_pins`
- `anim_blueprint_graph_get_view`
- `anim_blueprint_graph_set_view`
- `anim_blueprint_viewport_get_camera`
- `anim_blueprint_viewport_set_camera`
- `anim_blueprint_screenshot`

说明：

- 这组命令现在已经覆盖 AnimBlueprint 资产、逻辑图、Anim Layer、Layer Interface、State Machine 结构施工、Entry 控制、State/Transition 属性编辑、文件夹式导出/回写，以及图视图 / 预览视口 / 截图。
- 当前第一版文件夹式工作流里，成员与普通逻辑图复用 Blueprint proxy，AnimLayer / StateMachine 结构先重建，再回填动画子图。
- `BlendSpace 1D/2D` 当前已经能通过 AnimGraph 节点属性真源保留 `BlendSpace` 资产引用，但还没有 `BlendSpace` 资产本体的独立命令面。
- 仍未提供“任意 AnimGraph 节点的通用删除 / 连线”原语，也还没有处理会附带子图的结构型 AnimGraph 节点按类创建。
- `anim_blueprint_viewport_get_camera / set_camera` 和 `anim_blueprint_screenshot target=viewport` 更适合在真实 Editor 会话里验证。 

### 4.13 Animation Assets / Skeleton

- `anim_sequence_get_info`
  - 关键参数：`asset_path`
  - 返回：`sequence_length`、`num_frames`、`rate_scale`、`interpolation_type`、`additive_animation_type`、`additive_base_pose_sequence`、`retarget_source_asset`、`notifies[]`、`notify_tracks[]`、`sync_markers[]`
- `anim_sequence_screenshot`
  - 关键参数：`asset_path`
  - 可选：`skeletal_mesh_path`、`frame_index` 或 `time_seconds`、`format=png/jpg/webp`、`quality`、`max_size`、`target=viewport`
  - 返回：`file_path`、`capture_mode`、`frame_index`、`time_seconds`、`num_frames`、`preview_skeletal_mesh`
  - 说明：当前用于动画选帧审查，内部使用临时预览角色、受控补光和背景板，不依赖当前关卡相机
- `anim_sequence_set_curve`
  - 关键参数：`asset_path`、`curve_name`
  - 可选：`curve_type=float`、`curve_json`、`keys[]`、`time_seconds/value`、`clear_existing_keys`、`remove`
  - 说明：推荐用 `curve_json` 走 `ue_agent_interface.curve.v1`，写入前会返回未知字段、缺值、重复时间和非法插值模式等 `json_issues[]`
- `anim_sequence_set_bones`
  - 关键参数：`asset_path`
  - 可选：`remove_bone_names[]`、`include_children`、`children_excluded[]`、`exclude_children_recursively`、`remove_all_bone_animation`、`remove_virtual_bone_names[]`、`finalize_after_set`
- `anim_sequence_set_metadata`
  - 关键参数：`asset_path`
  - 可选：`metadata_class_path`、`metadata_values`、`remove`、`clear_all`
  - 返回：`metadata_count`、`metadata[]`
  - 说明：`metadata[].properties` 当前稳定回读简单可编辑属性（`FName/FString/FText/bool/数值`）
- `anim_sequence_set_notify`
  - 关键参数：`asset_path`
  - 新增时：`time_seconds`
  - 更新/删除时：`notify_index`
  - 可选：`track_name`、`notify_name`、`notify_class`、`notify_state_class`、`duration_seconds`、`tick_type`、`trigger/filter` 字段、`notify_color`、`remove`、`save_after_set`
- `anim_sequence_set_notify_track`
  - 关键参数：`asset_path`、`track_name`
  - 可选：`track_color`、`remove`、`save_after_set`
- `anim_sequence_set_sync_markers`
  - 关键参数：`asset_path`
  - 可选：`add_markers[]`、`remove_marker_names[]`、`remove_notify_track_names[]`、`clear_all`、`save_after_set`
- `skeleton_get_info`
  - 关键参数：`asset_path`
  - 返回：`root_bone_name`、`slot_groups[]`、`sockets[]`、`virtual_bones[]`、`compatible_skeletons[]`、`use_retarget_modes_from_compatible_skeleton`
- `skeleton_list_bones`
  - 关键参数：`asset_path`
- `skeleton_set_compatible_skeletons`
  - 关键参数：`asset_path`
  - 可选：`set_compatible_skeleton_paths[]`、`add_compatible_skeleton_paths[]`、`remove_compatible_skeleton_paths[]`、`clear_all`、`use_retarget_modes_from_compatible_skeleton`
- `skeleton_set_preview_mesh`
  - 关键参数：`asset_path`
  - 可选：`skeletal_mesh_path`、`clear_preview_mesh`、`save_after_set`
- `skeleton_set_socket`
  - 关键参数：`asset_path`、`socket_name`
  - 可选：`bone_name`、`relative_location/rotation/scale`、`remove`、`save_after_set`
- `skeleton_set_virtual_bone`
  - 关键参数：`asset_path`
  - 新增时：`source_bone_name`、`target_bone_name`
  - 删除时：`virtual_bone_name`、`remove=true`

说明：

- 这组命令当前已经覆盖 AnimSequence 的资产级设置、选帧截图、压缩 / 剥帧设置、float 曲线、骨骼轨删除、metadata 生命周期、普通 notify、notify track、sync marker，以及 Skeleton 的 compatible skeleton / preview mesh / socket / virtual bone 管理。
- 仍未扩到自定义 notify payload 属性编辑，也没有扩到完整 skeleton retarget profile 细粒度参数。
- AnimSequence 曲线当前稳定开放 `float curve`；通用曲线资产请使用 `curve_export_json / curve_apply_json`，已覆盖 `UCurveFloat / UCurveVector / UCurveLinearColor / UCurveTable`。

### 4.14 IK Rig / IK Retargeter

- `ik_rig_create`
  - 关键参数：`asset_path`
  - 可选：`preview_skeletal_mesh`、`apply_auto_retarget_definition`、`save_after_create`
- `ik_rig_get_info`
  - 关键参数：`asset_path`
  - 返回：`preview_skeletal_mesh`、`retarget_root`、`goals[]`、`retarget_chains[]`、`solvers[]`
  - 说明：`solvers[]` 当前已稳定补充 `BodyMover` 的 `settings` 与 `goal_settings[]` 摘要
- `ik_rig_export_folder`
- `ik_rig_validate_folder`
- `ik_rig_apply_folder`
  - 结构化文件：`preview.json`、`goals.json`、`retarget_definition.json`、`solvers.json`、`raw_properties.json`、`validation/*.json`
  - 覆盖：preview mesh、retarget root、goals、retarget chains、solver 基础结构，以及 BodyMover / FBIK 已开放设置
- `ik_rig_apply_auto_retarget_definition`
- `ik_rig_preview_solve`
  - 关键参数：`asset_path`
  - 可选：`skeletal_mesh_path`、`goals[]`、`sample_bones[]`、`include_all_bones`、`max_output_bones`
  - 返回：`initialized`、`solved`、`errors/warnings/messages`、`goals[]`、`output_pose_sample[]`
- `ik_retargeter_create`
  - 关键参数：`asset_path`
  - 可选：`source_ik_rig`、`target_ik_rig`、`source_preview_mesh`、`target_preview_mesh`、`add_default_ops`
- `ik_retargeter_get_info`
  - 关键参数：`asset_path`
  - 返回：`source_ik_rig`、`target_ik_rig`、`source_preview_mesh`、`target_preview_mesh`、`source_pose_names[]`、`target_pose_names[]`、`current_source_pose`、`current_target_pose`、`root_settings`、`global_settings`、`chain_settings[]`、`retarget_op_count`
- `ik_retargeter_export_folder`
- `ik_retargeter_validate_folder`
- `ik_retargeter_apply_folder`
  - 结构化文件：`rigs.json`、`preview_meshes.json`、`global_settings.json`、`root_settings.json`、`chain_mappings.json`、`chain_settings.json`、`poses/*/*.json`、`validation/*.json`
  - 覆盖：source / target IK Rig、source / target preview mesh、global / root / chain settings、chain mapping、默认 source / target pose
- `ik_retargeter_auto_map_chains`
  - 可选：`auto_map_type=exact/fuzzy/clear`、`force_remap`、`op_name`
- `ik_retargeter_auto_align_pose`
  - 关键参数：`asset_path`、`source_or_target`
  - 可选：`pose_name`、`create_if_missing`、`set_current`、`auto_align_method`、`auto_align_all`、`auto_align_bones[]/bones[]`、`snap_bone_to_ground`、`reset_all`、`reset_bones[]`、`root_offset`
- `ik_retargeter_duplicate_and_retarget`
  - 关键参数：`asset_path`、`asset_paths[]`、`output_folder`
  - 可选：`source_mesh_path`、`target_mesh_path`、`prefix/suffix/search/replace`、`include_referenced_assets`
- `retarget_batch_export_json`
- `retarget_batch_validate_json`
- `retarget_batch_apply_json`
  - 批量重定向是动作型 JSON，不属于普通 IK Retargeter 资产状态

说明：

- 当前优先保证 IK / Retarget 的主流程：创建资产、导出 folder JSON、修改真实结构、校验、回写、读回。
- 已能被 `ik_rig_apply_folder` / `ik_retargeter_apply_folder` 覆盖的旧 `set_*` 写入命令已移动到 `commands/deprecatedCommand/14_IKRig_IKRetargeter.md`，只用于 bootstrap、迁移、schema 边界、局部补修和故障恢复。
- `ik_rig_preview_solve`、`ik_retargeter_auto_align_pose`、`ik_retargeter_auto_map_chains`、`ik_retargeter_duplicate_and_retarget` 与 `retarget_batch_*` 是动作语义，仍保留在主流程中，但不隐式塞进普通 folder apply。

### 4.6 StaticMesh

- `static_mesh_open_editor`
- `static_mesh_get_info`
- `static_mesh_get_bounds`
- `static_mesh_get_local_corners`
- `static_mesh_set_preview_view`
- `static_mesh_set_material_slot`
- `static_mesh_set_collision_boxes`
- `static_mesh_set_collision_spheres`
- `static_mesh_set_collision_capsules`
- `static_mesh_add_socket`
- `static_mesh_update_socket`
- `static_mesh_remove_socket`
- `static_mesh_reimport`
- `static_mesh_build`
- `static_mesh_preview_collision`

StaticMesh 补充：
- `static_mesh_get_info`：原本已经返回 bounds；现在补了两个更明确的拆分命令，便于脚本直接取几何信息。
- `static_mesh_get_info`：当前也会返回 `material_slots[]`、`collision.spheres[]`、`collision.capsules[]`，适合做默认材质槽与简单碰撞回读。
- `static_mesh_get_bounds`：只读取静态网格 bounds；参数 `asset_path`。
- `static_mesh_get_local_corners`：读取静态网格局部包围盒 8 个角点；参数 `asset_path`。
- `static_mesh_set_material_slot`：设置默认材质槽材质；参数 `asset_path`、`material_path`，以及 `slot_index` 或 `slot_name`。
- `static_mesh_set_collision_boxes / spheres / capsules`：分别重设三类简单碰撞形状数组；都支持 `clear_other_shapes`。
- `static_mesh_reimport`：使用 UE reimport handler 自动化重导入；参数 `asset_path`，可选 `source_filename/save_after_reimport/show_notification`。
- `static_mesh_preview_collision`：无 UI 返回 simple collision 与 bounds 摘要，后续用 `level_trace_world_ray / level_sweep_capsule / level_check_overlaps` 做场景验证。

### 4.7 EnhancedInput

- 当前 `InputAction / InputMappingContext` 默认优先使用单文件 JSON 工作流。
- EnhancedInput 废弃写入命令明细见 `commands/deprecatedCommand/04_StaticMesh_EnhancedInput.md`，默认只做初始化、探针验证和局部补修。
- `enhanced_input_create_action`
- `enhanced_input_get_action_info`
- `enhanced_input_export_action_json`
- `enhanced_input_apply_action_json`
- `enhanced_input_create_mapping_context`
- `enhanced_input_get_mapping_context_info`
- `enhanced_input_export_mapping_context_json`
- `enhanced_input_apply_mapping_context_json`

### 4.8 Modeling Mode

- `modeling_activate_mode`
- `modeling_get_selection`
- `modeling_set_selection`
- `modeling_set_mesh_selection_mode`
- `modeling_get_mesh_selection_info`
- `modeling_clear_mesh_selection`
- `modeling_select_mesh_elements_via_screen`
- `modeling_select_mesh_elements_via_world_ray`
- `modeling_start_tool`
- `modeling_get_active_tool`
- `modeling_get_active_tool_properties`
- `modeling_set_active_tool_property`
- `modeling_invoke_active_tool_action`
- `modeling_accept_tool`
- `modeling_cancel_tool`
- `modeling_convert_actor_to_dynamic_mesh`
- `modeling_duplicate_to_new_static_mesh`
- `modeling_create_box`
- `modeling_create_cylinder`
- `modeling_create_sphere`
- `modeling_create_plane`
- `modeling_create_stairs`
- `modeling_create_ramp`
- `modeling_create_ramp_corner`
- `modeling_extrude_faces`
- `modeling_inset_faces`
- `modeling_bevel_edges`
- `modeling_offset`
- `modeling_push_pull`
- `modeling_mirror`
- `modeling_duplicate_faces`
- `modeling_boolean`
- `modeling_trim`
- `modeling_plane_cut`
- `modeling_mesh_cut`
- `modeling_voxel_boolean`
- `modeling_remesh`
- `modeling_simplify`
- `modeling_subdivide`
- `modeling_weld_edges`
- `modeling_fill_holes`
- `modeling_recompute_normals`
- `modeling_set_pivot`
- `modeling_bake_transform`
- `modeling_align_to_world`
- `modeling_snap_to_ground`
- `modeling_auto_uv`
- `modeling_project_uv`
- `modeling_set_material_slot`
- `modeling_add_material_slot`
- `modeling_remove_material_slot`
- `modeling_generate_simple_collision`
- `modeling_generate_convex_collision`
- `modeling_save_mesh_asset`
- `modeling_replace_actor_mesh`

Modeling 补充：
- `modeling_activate_mode`：激活 `Modeling` 编辑模式。
- `modeling_get_selection` / `modeling_set_selection`：读取/设置建模目标 Actor；`modeling_set_selection` 支持 `sync_geometry_targets`。
- `modeling_set_mesh_selection_mode`：设置几何元素选择模式；参数 `element_type=vertex/edge/face`、`topology_mode=triangle/polygroup/none`、可选 `convert_existing_selection`。
- `modeling_get_mesh_selection_info`：读取当前几何选择模式与目标数量。
- `modeling_clear_mesh_selection`：清空当前几何元素选择。
- `modeling_select_mesh_elements_via_screen`：按屏幕坐标拾取几何元素；参数 `screen_x`、`screen_y`，可选 `change_type`、`clear_on_miss`。
- `modeling_select_mesh_elements_via_world_ray`：按世界射线拾取几何元素；参数 `world_origin`、`world_direction`，可选 `change_type`、`clear_on_miss`。
- `modeling_start_tool`：启动任意建模工具；参数 `tool_identifier`，可选 `accept`、`post_action`、`property_set`、`property_name`、`value_text`。
- `modeling_get_active_tool` / `modeling_get_active_tool_properties`：读取当前活动工具与属性集摘要。
- `modeling_set_active_tool_property`：设置当前活动工具属性；参数 `property_set`、`property_name`、`value_text`。
- `modeling_invoke_active_tool_action`：调用当前工具或属性集上的无参动作；参数 `action_name`、可选 `property_set`。
- `modeling_accept_tool` / `modeling_cancel_tool`：接受/取消当前工具结果。
- `modeling_save_mesh_asset`：保存当前选择 Actor 引用的静态网格资产；参数 `actor_id`。
- `modeling_replace_actor_mesh`：替换 Actor 的静态网格；参数 `actor_id`、`static_mesh_asset`。
- `modeling_snap_to_ground`：将 Actor 简单吸附到地面；参数 `actor_id`、可选 `trace_distance`、`ground_offset`。
- 当前已知边界：
  - `modeling_*` 依赖 Editor viewport、active tool 和 selection context，不能视为完全 headless。
  - 对强交互工具，推荐分阶段执行并在每一步读取活动工具状态。
- 下列封装命令内部仍走建模工具：
  - 创建：`modeling_create_box`、`modeling_create_cylinder`、`modeling_create_sphere`、`modeling_create_plane`、`modeling_create_stairs`、`modeling_create_ramp`、`modeling_create_ramp_corner`
  - 资产与目标：`modeling_convert_actor_to_dynamic_mesh`、`modeling_duplicate_to_new_static_mesh`
  - 变形：`modeling_extrude_faces`、`modeling_inset_faces`、`modeling_bevel_edges`、`modeling_offset`、`modeling_push_pull`、`modeling_mirror`、`modeling_duplicate_faces`
  - 切割与布尔：`modeling_boolean`、`modeling_trim`、`modeling_plane_cut`、`modeling_mesh_cut`、`modeling_voxel_boolean`
  - 拓扑与重构：`modeling_remesh`、`modeling_simplify`、`modeling_subdivide`、`modeling_weld_edges`、`modeling_fill_holes`、`modeling_recompute_normals`
  - 变换与对齐：`modeling_set_pivot`、`modeling_bake_transform`、`modeling_align_to_world`
  - UV / 材质 / 碰撞：`modeling_auto_uv`、`modeling_project_uv`、`modeling_set_material_slot`、`modeling_add_material_slot`、`modeling_remove_material_slot`、`modeling_generate_simple_collision`、`modeling_generate_convex_collision`
- 对 `modeling_create_box / cylinder / sphere / plane / stairs / ramp / ramp_corner`，当前还支持可选 `folder_path`：创建并接受工具后会直接把新 Actor 放进指定 Outliner folder，适合配合 `batch` 和 `level_destroy_folder_actors` 做整组白盒迭代。
- `modeling_create_ramp`：创建自定义斜坡基础元件，几何为沿局部 `+X` 抬升、沿局部 `Y` 展开的三棱柱。
- `modeling_create_ramp_corner`：创建自定义 `rampCorner` 基础元件，几何为底面是直角矩形、顶点位于 `(+X,+Y,+Z)` 的四棱锥，用作斜坡与转角过渡件。
- `modeling_create_ramp / ramp_corner` 在 `bounds` 求解链里与 `box / plane` 一样按 `Depth -> X`、`Width -> Y`、`Height -> Z` 推导尺寸，并在接受后把 Actor 放到 `bounds.center + rotation`。

### 4.9 Material

- `material_create`
- `material_instance_create`
- `material_open_editor`
- `material_get_info`
- `material_compile`
- `material_get_compile_log`
- `material_export_folder`
- `material_apply_folder`
- `material_instance_export_folder`
- `material_instance_apply_folder`
- `material_function_export_folder`
- `material_function_apply_folder`
- `material_list_expressions`

#### Blueprint / UMG / Material 编译日志相关参数

- `blueprint_compile` / `umg_compile`：
  - 可选：`include_messages`（默认 `false`）、`severity_filter`（`all/error/warning/warning_or_error/info`）、`max_messages`（默认 `200`）
  - 返回：`error_count`、`warning_count`、`has_error`，若 `include_messages=true` 还会返回 `messages`
- `blueprint_get_compile_log` / `umg_get_compile_log`：
  - 关键参数：`asset_path`
  - 可选：`severity_filter`、`max_messages`、`save_after_compile`
  - 说明：该命令会触发一次编译并返回过滤后的编译消息列表
- `material_compile`：
  - 可选：`include_messages`（默认 `false`）、`severity_filter`、`max_messages`、`save_after_compile`
  - 返回：`error_count`、`has_error`，若 `include_messages=true` 返回 `messages`
- `material_get_compile_log`：
  - 关键参数：`asset_path`
  - 可选：`compile_before_read`（默认 `true`）、`severity_filter`、`max_messages`、`save_after_compile`
  - 返回：按 FeatureLevel 聚合的编译错误（`messages[].feature_level` + `messages[].message`）
- `material_export_folder / material_apply_folder`：
  - 当前这是 `UMaterial` 的主编辑路径
  - 当前只覆盖 `UMaterial`
  - 固定导出根目录：`Saved/UeAssetFolders/MaterialGraph`
  - 单个资产目录按 `asset_path` 自动展开，例如 `/Game/Materials/M_Door` -> `Saved/UeAssetFolders/MaterialGraph/Game/Materials/M_Door`
  - `apply_folder` 当前对表达式图采用“删除现有表达式，再按文件夹描述重建”的方式
  - `settings/material.json` 对应材质根对象左侧 `Details` 面板
- `material_instance_export_folder / material_instance_apply_folder`：
  - 当前这是 `UMaterialInstanceConstant` 的主编辑路径
  - 当前只覆盖 `UMaterialInstanceConstant`
  - 固定导出根目录：`Saved/UeAssetFolders/MaterialInstance`
  - 单个资产目录按 `asset_path` 自动展开，例如 `/Game/Materials/MI_Door` -> `Saved/UeAssetFolders/MaterialInstance/Game/Materials/MI_Door`
  - 目录结构：`asset.json`、`parent.json`、`parameters/overrides.json`、`validation/checks.json`
  - `apply_folder` 第一版聚焦“父材质 + 参数 overrides”，默认 `clear_existing_overrides=true`
- `material_function_export_folder / material_function_apply_folder`：
  - 当前这是 `UMaterialFunction` 的主编辑路径
  - 当前只覆盖 `UMaterialFunction`
  - 固定导出根目录：`Saved/UeAssetFolders/MaterialFunction`
  - 单个资产目录按 `asset_path` 自动展开，例如 `/Game/Materials/MF_Waves` -> `Saved/UeAssetFolders/MaterialFunction/Game/Materials/MF_Waves`
  - 目录结构：`asset.json`、`function.json`、`graphs/MaterialFunctionGraph.json`、`validation/checks.json`
  - `apply_folder` 第一版采用“删除现有表达式，再按文件夹描述重建”的方式，完成后调用 `UpdateMaterialFunction`
- `material_get_info`：
  - 当前除 `scalar/vector/texture/static_switch` 外，还会返回 `static_component_mask_parameters[]`
- Material 废弃写入命令明细见 `commands/deprecatedCommand/05_Material.md`。

### 4.10 Sequence（Level Sequence + UMG Animation）

- `sequence_list_level_sequences`
  - 关键参数：可选 `root_path`、`limit`
- `sequence_create_level_sequence`
  - 关键参数：`asset_path`、`start_seconds`、`duration_seconds`、`display_rate_num`、`display_rate_den`、`open_editor`、`save_after_create`
- `sequence_open_level_sequence`
  - 关键参数：`asset_path`
- `sequence_get_level_sequence_info`
  - 关键参数：`asset_path`
- `sequence_export_folder`
  - 关键参数：`asset_path`
  - 当前这是 `Level Sequence` 的主编辑路径
  - 固定导出根目录：`Saved/UeAssetFolders/LevelSequence`
  - 当前稳定导出结构：
    - `asset.json`
    - `settings/sequence.json`
    - `bindings/index.json`
    - `bindings/<Binding>/binding.json`
    - `bindings/<Binding>/tracks/*.json`
    - `outliner/folders.json`
    - `master_tracks/index.json`
    - `validation/checks.json`
- `sequence_apply_folder`
  - 关键参数：`asset_path`
  - 从 `Saved/UeAssetFolders/LevelSequence/<asset_path>` 读取结构化目录并回写
  - 当前稳定回写类型：
    - `spawn`
    - `transform`
    - `skeletal_animation`
    - `visibility`
    - `property(float/double/bool/integer/color/byte/string/vector/rotator/actor_reference/object)`
  - 当前会优先复用既有 `binding_guid`
  - 若 binding 丢失，会按 `binding.json` 自动恢复常见：
    - `Actor possessable`
    - `ActorComponent possessable`
    - `Actor spawnable`
  - `outliner/folders.json` 当前已纳入第一版稳定 apply：
    - 根 folder / 子 folder
    - `child_binding_guids`
    - `child_binding_tracks`
      - `spawn / transform / skeletal_animation / visibility / property(float/double/bool/integer/color)`
    - `child_master_tracks(camera_cut / sub_sequence / cinematic_shot)`
  - `master_tracks/index.json` 当前已纳入第一版稳定 apply：
    - `camera_cut`
    - `sub_sequence`
    - `cinematic_shot`
  - 播放设置、binding、track、key、section 和 UMG Animation 废弃写入命令明细见 `commands/deprecatedCommand/06_Sequence.md`；完整 authoring 优先写入 folder JSON
- `sequence_list_umg_animations`
  - 关键参数：`asset_path`
- `sequence_get_umg_animation_info`
  - 关键参数：`asset_path`、`animation_name`
  - 返回：`display_rate`、`tick_resolution`、`bindings[].tracks[]`

### 4.11 Montage

- `montage_list_montages`
  - 关键参数：可选 `root_path`、`limit`
- `montage_create`
  - 关键参数：`asset_path`
  - 可选：`target_skeleton`、`source_animation`、`preview_skeletal_mesh`、`open_editor`、`save_after_create`
- `montage_open_editor`
  - 关键参数：`asset_path`
- `montage_get_info`
  - 关键参数：`asset_path`
  - 返回：`slot_tracks[]`、`sections[]`、`notify_tracks[]`、`notifies[]`、`sync_markers[]`、`blend_*`、`sync_*`、`sequence_length`
- `montage_export_json`
  - 关键参数：`asset_path`
  - 可选：`output_file`
  - 当前这是 `AnimMontage` 的主编辑路径
  - 当前稳定 JSON 根字段：
    - `settings`
    - `sync`
    - `slot_tracks`
    - `sections`
    - `notify_tracks`
    - `notifies`
    - `sync_markers`
    - `skeleton_slots`
- `montage_apply_json`
  - 关键参数：`json_file`
  - 可选：`create_if_missing`、`save_after_apply`
  - 当前会按“先补缺口、再删多余、最后重建内容”的顺序回写 slot/notify 结构，避免卡在 UE 对“最后一个 track 不允许直接删除”的约束上
  - preview/blend/sync、Skeleton slot、notify、slot track、segment、section 废弃写入命令明细见 `commands/deprecatedCommand/12_Montage.md`；完整 authoring 优先写入 Montage JSON
- `montage_list_skeleton_slots`
  - 关键参数：`skeleton_path` 或 `asset_path`

### 4.12 Niagara

- `niagara_list_assets`
  - 关键参数：可选 `root_path`、`limit`
- `niagara_list_module_library`
  - 关键参数：可选 `root_path`、`category`、`name_contains`、`limit`、`include_non_module_scripts`
  - 返回：内置 Niagara Module Library 条目，包含 `asset_path`、`category`、`relative_package_path`、`script_usage`、`supported_usages`、`library_visibility`
- `niagara_create_system`
  - 关键参数：`asset_path`，可选：`create_default_nodes`、`open_editor`、`save_after_create`
  - 返回：若同路径存在已删除但尚未 GC 的内存残影，会返回 `found_stale_object_before_create` / `detached_stale_object_before_create`
- `niagara_create_emitter`
  - 关键参数：`asset_path`，可选：`add_default_modules_and_renderers`、`open_editor`、`save_after_create`
  - 返回：若同路径存在已删除但尚未 GC 的内存残影，会返回 `found_stale_object_before_create` / `detached_stale_object_before_create`
- `niagara_delete_asset`
    - 关键参数：`asset_path`；可选：`force_delete`、`use_unchecked_delete`
    - 返回：`deleted_asset_path`、`asset_kind`、`delete_strategy`、`found_residual_object_after_delete`、`detached_residual_object_after_delete`
    - 说明：删除 Niagara System / Emitter / Script。`force_delete=true` 会走无确认删除；未落盘临时资产会自动使用 `delete_objects_unchecked`，避免临时 NiagaraScript 在 UE 引用替换路径中长时间阻塞服务。删除成功后会清理同路径残留内存对象，支持同会话内删除后立即重建
- `niagara_duplicate_asset`
  - 关键参数：`source_asset_path`、`target_asset_path`，可选：`open_editor`、`save_after_create`
  - 返回：若目标路径存在已删除但尚未 GC 的内存残影，会返回 `found_stale_target_object_before_duplicate` / `detached_stale_target_object_before_duplicate`
- `niagara_open_editor`
  - 关键参数：`asset_path`
- `niagara_preview_advance`
    - 关键参数：`asset_path`，可选：`open_editor_if_needed`、`reset_preview`、`target_frame/frame/advance_frames/preview_advance_frames`、`target_time_seconds/preview_advance_seconds`、`tick_delta_seconds/preview_tick_delta_seconds`、`advance_mode`、`pause_after_advance`
    - 返回：`preview_state_token`、`target_frame`、`advanced_frame_count`、`system_age`、`current_frame_estimate`、`advance_semantics`、`used_seek=false`、目标帧 `stats`
    - 说明：从 0 或当前预览状态连续 tick 到目标帧并暂停，不启动 PIE / game。Collision Event / Death Event / Event Handler 类效果推荐先用该命令推进一次，再用 current-preview 的 probe 和 screenshot 只读同一状态
- `niagara_screenshot`
    - 关键参数：`asset_path`，可选：`target=window|viewport`、`open_editor_if_needed`、`offscreen/use_offscreen_renderer`、`capture_mode=current_preview`、`expected_preview_state_token`、`expected_frame`、`reset_preview`、`preview_advance_seconds`、`format`、`quality`、`max_size`、`file_path`
    - 返回：截图 `file_path`、尺寸、字节数、`capture_mode`、`preview_prepare_requested`、`current_preview_validation`、`pre_preview_redraw_performed`、`capture_redraw_performed`
    - 说明：用于调试 Niagara 编辑器界面是否真实写入；只截图编辑器 UI，不运行 PIE / game。`capture_mode=current_preview` 为严格只读当前暂停预览，不 reset、不 activate、不 tick，建议配合 `niagara_preview_advance` 返回的 `preview_state_token`
- `niagara_system_runtime_probe`
    - 关键参数：`asset_path`
    - 可选参数：`open_editor_if_needed`、`sample_mode=current_preview`、`expected_preview_state_token`、`expected_frame`、`reset_preview`、`tick_count`、`tick_delta_seconds`、`advance_mode`、`include_script_runtime_stats`、`include_snapshots`
    - 返回：`initial_stats`、`final_stats`、`summary.emitter_peaks[]`、`current_preview_validation`、可选 `snapshots[]`、System / Emitter 执行状态、粒子数和生成数
    - 说明：不启动 PIE / game 窗口。默认 simulate 模式会推进 Niagara 编辑器预览组件；`sample_mode=current_preview` 为严格只读当前暂停预览，不 reset、不 activate、不 tick。Collision Event / Death Event / 短 lifetime 粒子推荐流程：`niagara_preview_advance` 连续推进并暂停，再用 current-preview probe 读取同一状态
- `niagara_get_info`
    - 关键参数：`asset_path`
- `niagara_get_stack_issues`
  - 关键参数：`asset_path`
  - 可选参数：`severity_filter`（`warning_or_error/all/error/warning/info/none`）、`prefer_existing_view_model`、`open_editor_if_needed`、`include_system_scope`、`include_emitters`、`emitter_name`、`emitter_id`、`emitter_index`、`compile_before_read`
  - 返回：`issues[]`、`scopes[]`、`scope_count`、`total_error_count`、`total_warning_count`、`total_info_count`、`view_model_source`
  - 说明：读取 Niagara Stack 面板 `UNiagaraStackEntry::FStackIssue`，并合并 Niagara `FNiagaraMessageStore` 中会显示到 Stack 的资产消息，用于获取红色感叹号/黄色警告/信息提示的 `short_description`、`long_description`、`entry_path`、`module_node_guid`、`module_script_asset_path` 和 `fixes[]`。`scopes[]` 额外返回按 UE `SNiagaraStackIssueIcon` 聚合的 `stack_issue_icon_kind`、`stack_issue_tooltip_summary`、`message_store_added_issue_count`，以及 emitter 编译/状态提示字段 `emitter_latest_compile_status`、`emitter_handle_error_text`、`emitter_handle_error_visibility`。MessageStore 来源的 issue 会标记 `issue_origin=message_store`，并包含 `message_key`、`message_topic`、`message_source_path`。要匹配 UI 悬停文本，建议 `prefer_existing_view_model=true` + `open_editor_if_needed=true`；未打开编辑器时会退回 data-processing ViewModel，但可能漏掉 UI-only / live Stack 状态
- `niagara_apply_stack_issue_fix`
  - 关键参数：`asset_path`，并用 `issue_unique_identifier` / `module_node_guid` / `entry_path` / `scope_name` / `emitter_name` / `emitter_id` / `emitter_index` 等 selector 缩小匹配
  - 可选参数：`fix_index`、`fix_unique_identifier`、`compile_after_apply`、`force_compile`、`wait_for_complete`、`save_after_apply`
  - 说明：执行 `niagara_get_stack_issues` 返回的 UE Stack Quick Fix；应用后会刷新 owning System
- `niagara_export_folder`
    - 关键参数：`asset_path`
    - 可选参数：`folder_path`、`clean_output_dir`
    - 返回：`folder_path`、`file_count`、`emitter_count`、`user_parameter_count`、`script_reference_count`、`warnings`
    - 说明：导出 `UNiagaraSystem -> FNiagaraEmitterHandle -> FVersionedNiagaraEmitter -> UNiagaraEmitter` 文件夹结构；`emitters/index.json` 会区分 standalone emitter asset 与 system 内嵌 emitter instance，并写入 `validation/coverage_report.json`
- `niagara_apply_folder`
    - 关键参数：`folder_path` 或 `asset_path`
    - 可选参数：`create_if_missing`、`apply_referenced_emitters`、`compile_after_apply`、`wait_for_complete`、`save_after_apply`、`collect_stack_issues_after_apply`、`stack_issue_severity_filter`、`prefer_existing_stack_view_model`、`open_editor_for_stack_issues`、`compile_before_stack_issue_read`、`fail_on_stack_errors`、`strict`
    - 返回：System 属性、User 参数、Emitter Handle、Emitter 属性的回写数量，coverage warnings，以及 `stack_issue_report`、`stack_issues`、`stack_scopes`、`stack_error_count`、`stack_warning_count`
    - 说明：支持结构化与通用属性回写；内嵌 emitter 没有 standalone 源资产时会生成模板 handle 后应用文件夹数据；Emitter Stage/Module/Event/Renderer/Script 引用会按文件夹内容回写。Data Interface 的曲线 `raw_properties` 使用 `ue_agent_interface.curve.v1`，System 总刷新后再回写，返回 `post_refresh_data_interfaces_applied`。默认 apply 后读取 Niagara Stack 感叹号内容；`strict=true` 可把运行时 warning 或 Stack error 视为失败
- `niagara_emitter_export_folder`
    - 关键参数：`emitter_asset_path`（或 `asset_path`）
    - 可选参数：`folder_path`、`clean_output_dir`
    - 返回：`folder_path`、`file_count`、`script_reference_count`、`warnings`
    - 说明：导出 standalone `UNiagaraEmitter` 文件夹结构，固定根目录 `Saved/UeAssetFolders/NiagaraEmitter`
- `niagara_emitter_apply_folder`
    - 关键参数：`folder_path` 或 `emitter_asset_path`
    - 可选参数：`create_if_missing`、`add_default_modules_and_renderers`、`save_after_apply`、`collect_stack_issues_after_apply`、`stack_issue_severity_filter`、`prefer_existing_stack_view_model`、`open_editor_for_stack_issues`、`compile_before_stack_issue_read`、`fail_on_stack_errors`、`strict`
    - 返回：Emitter 属性回写数量、coverage warnings，以及 `stack_issue_report`、`stack_issues`、`stack_scopes`、`stack_error_count`、`stack_warning_count`
    - 说明：支持 standalone Emitter version data、graph 参数、rapid iteration 参数、Renderer、Event Generator、Event Handler、Simulation Stage、Module Stack、Module Input default、Data Interface 引用与 `raw_properties` 回写。曲线 raw property 优先使用 `curve_json`，`value_json` 仅作兼容别名。默认 apply 后读取 Emitter Stack 感叹号内容；`strict=true` 遇到 Stack error 会返回 `strict_apply_has_stack_errors`
- `niagara_script_export_folder`
    - 关键参数：`script_asset_path`（或 `asset_path`）
    - 可选参数：`folder_path`、`clean_output_dir`
    - 返回：`folder_path`、`file_count`、`node_count`、`link_count`、`warnings`
    - 说明：导出 standalone `UNiagaraScript` 文件夹结构，固定根目录 `Saved/UeAssetFolders/NiagaraScript`
- `niagara_script_apply_folder`
    - 关键参数：`folder_path` 或 `script_asset_path`
    - 可选参数：`create_if_missing`、`compile_after_apply`、`save_after_apply`、`strict`
    - 返回：Script 属性和节点属性回写数量、编译请求状态、coverage warnings
    - 说明：支持创建或更新 Niagara Script，回写脚本元数据、raw reflected fields、graph nodes 与 Custom HLSL 文本

> Niagara 废弃写入命令已迁移到 `commands/deprecatedCommand/07_Niagara_System.md`、`commands/deprecatedCommand/08_Niagara_Emitter.md` 和 `commands/deprecatedCommand/09_Niagara_StageGraph.md`。
- `niagara_system_get_property`
  - 关键参数：`asset_path`、`property_name`
- `niagara_set_property`
  - 关键参数：`asset_path`、`property_name`、`value_text`，可选：`save_after_set`
- `niagara_refresh_system`
  - 关键参数：`asset_path`
  - 可选参数：`broadcast_post_edit_change`、`mark_dirty`、`compile_after_refresh`、`force_compile`、`wait_for_complete`、`save_after_refresh`
  - 返回：`system_refresh`、编译请求/完成状态、`is_ready_to_run`
  - 说明：用于 UAI 结构化或原子写入后刷新 Niagara System 的 overview graph、emitter execution order、cached traversal data 和已打开的 ViewModel；`niagara_compile_system` 对 System 默认会先执行同类刷新。`mark_dirty=false` 时若 UE 内部刷新临时标脏原本 clean 的包，会恢复 dirty 状态并返回 `restored_dirty_state=true`
- `niagara_compile_system`
  - 关键参数：`asset_path`，可选：`force_compile`、`wait_for_complete`、`refresh_before_compile`、`mark_dirty_after_compile`、`save_after_compile`
  - 说明：默认用于校验，不主动标记资产 dirty；`save_after_compile=true` 会隐式启用 `mark_dirty_after_compile`。若 UE 内部编译临时标脏原本 clean 的包，会恢复 dirty 状态并返回 `restored_dirty_state=true`
- `niagara_get_compile_log`
  - 关键参数：`asset_path`
  - 可选参数：`compile_before_read`、`refresh_before_compile`、`force_compile`、`wait_for_complete`、`severity_filter`（`all/error/warning/warning_or_error/info`）、`include_events`、`include_stack_guids`、`max_events_per_script`、`include_disabled_emitters`
  - 返回：每个 Niagara Script 的 `compile_status`、`error_msg`、`LastCompileEvents`（可过滤严重级别）以及聚合错误/警告统计
  - 说明：`compile_before_read=true` 的读前刷新不会主动标记 System dirty；若 UE 内部读前编译临时标脏原本 clean 的包，会恢复 dirty 状态并返回 `restored_dirty_state=true`
- `niagara_user_parameter_list`
  - 关键参数：`asset_path`；可选：`include_values`
- `niagara_user_parameter_add`
  - 关键参数：`asset_path`、`parameter_name`、`parameter_type`；可选：`default_value_text`（或 `default_value`）、`compile_after_set`、`wait_for_complete`、`save_after_set`
- `niagara_user_parameter_remove`
  - 关键参数：`asset_path`、`parameter_name`；可选：`compile_after_set`、`wait_for_complete`、`save_after_set`
- `niagara_user_parameter_get`
  - 关键参数：`asset_path`、`parameter_name`
- `niagara_user_parameter_set`
  - 关键参数：`asset_path`、`parameter_name`、`value_text`（或 `default_value_text`）；可选：`compile_after_set`、`wait_for_complete`、`save_after_set`
- `niagara_system_list_emitters`
  - 关键参数：`asset_path`
- `niagara_system_add_emitter`
  - 关键参数：`system_asset_path`、`emitter_asset_path`，可选：`emitter_name`、`emitter_version`、`compile_after_set`、`wait_for_complete`、`save_after_set`
- `niagara_system_remove_emitter`
  - 关键参数：`system_asset_path`，并提供其一：`emitter_id` / `emitter_name` / `emitter_index`；可选：`compile_after_set`、`wait_for_complete`、`save_after_set`
- `niagara_system_move_emitter`
  - 关键参数：`system_asset_path` + 发射器标识（`emitter_id` / `emitter_name` / `emitter_index`）+ `target_index`；可选：`compile_after_set`、`wait_for_complete`、`save_after_set`
- `niagara_system_set_emitter_enabled`
  - 关键参数：`system_asset_path` + 发射器标识（`emitter_id` / `emitter_name` / `emitter_index`）+ `enabled`；可选：`compile_after_set`、`wait_for_complete`、`save_after_set`
- `niagara_system_set_emitter_version`
  - 关键参数：`system_asset_path` + 发射器标识（`emitter_id` / `emitter_name` / `emitter_index`）+ `emitter_version`；可选：`compile_after_set`、`wait_for_complete`、`save_after_set`
- `niagara_emitter_clear_parent`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）
  - 可选参数：`clear_merge_message`、`force`、`compile_after_apply`、`force_compile`、`wait_for_complete`、`save_after_apply`
  - 说明：清除 Emitter `Parent` / `ParentAtLastMerge` 和可选的 parent merge MessageStore 残留；对 System 内嵌 emitter 会刷新 owning System
- `niagara_emitter_get_property`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）+ `property_name`；可选：`emitter_version`
- `niagara_emitter_set_property`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）+ `property_name` + `value_text`
  - 可选：`emitter_version`、`compile_after_set`、`wait_for_complete`、`save_after_set`
  - 说明：用于设置发射器版本数据中的属性（例如 `bRequiresPersistentIDs`）
- `niagara_emitter_list_renderers`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）；可选：`emitter_version`、`include_properties`
- `niagara_emitter_add_renderer`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）+ `renderer_class`（或 `renderer_type`=`sprite/ribbon/mesh/light/decal`）；可选：`emitter_version`、`renderer_name`、`property_name`、`value_text`、`compile_after_set`、`wait_for_complete`、`save_after_set`
  - 如果初始 `property_name/value_text` 写入失败，会移除刚创建的 renderer，并返回 `renderer_removed_after_property_failure=true`
- `niagara_emitter_remove_renderer`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）+ 渲染器标识（`renderer_index` / `renderer_name` / `renderer_class`）；可选：`emitter_version`、`compile_after_set`、`wait_for_complete`、`save_after_set`
- `niagara_emitter_move_renderer`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）+ 渲染器标识（`renderer_index` / `renderer_name` / `renderer_class`）+ `target_index`
  - 可选：`emitter_version`、`compile_after_set`、`wait_for_complete`、`save_after_set`
- `niagara_emitter_get_renderer_property`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）+ 渲染器标识（`renderer_index` / `renderer_name` / `renderer_class`）+ `property_name`；可选：`emitter_version`
- `niagara_emitter_set_renderer_property`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）+ 渲染器标识（`renderer_index` / `renderer_name` / `renderer_class`）+ `property_name` + `value_text`；可选：`emitter_version`、`compile_after_set`、`wait_for_complete`、`save_after_set`
- `niagara_emitter_list_event_handlers`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）；可选：`emitter_version`
- `niagara_emitter_add_event_handler`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）；可选：`emitter_version`、`script_usage_id`、`source_emitter_id`、`source_event_name`、`execution_mode`、`spawn_number`、`min_spawn_number`、`max_events_per_frame`、`random_spawn_number`、`update_attribute_initial_values`、`compile_after_set`、`wait_for_complete`、`save_after_set`
  - 当前行为：会自动初始化对应 `ParticleEventScript` 的基础图结构（InputMap -> Output），避免“输出未连接”导致的编译错误
- `niagara_emitter_remove_event_handler`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）+ 事件标识（`event_index` / `script_usage_id` / `source_event_name`）；可选：`emitter_version`、`compile_after_set`、`wait_for_complete`、`save_after_set`
- `niagara_emitter_get_event_handler_property`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）+ 事件标识（`event_index` / `script_usage_id` / `source_event_name`）+ `property_name`；可选：`emitter_version`
- `niagara_emitter_set_event_handler_property`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）+ 事件标识（`event_index` / `script_usage_id` / `source_event_name`）+ `property_name` + `value_text`；可选：`emitter_version`、`compile_after_set`、`wait_for_complete`、`save_after_set`
  - `SourceEmitterID` 写入兼容：
    - 支持纯 GUID 文本（例如 `0e637b92-cdb2-4a45-bf47-f7efa7874e89`）
    - 也支持 UE Struct ImportText（例如 `(A=123,B=456,C=789,D=1011)`）
  - 建议先用 `niagara_emitter_list_event_handlers` 读取目标 Leader 的 `source_emitter_id`，再回填给 Follower 的 `SourceEmitterID`
- `niagara_emitter_parameter_list`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）；可选：`emitter_version`、`namespace`、`include_default_values`
- `niagara_emitter_parameter_add`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）、`parameter_name`、`parameter_type`；可选：`emitter_version`、`default_value_text`（或 `default_value`）、`is_static_switch`、`save_after_set`
- `niagara_emitter_parameter_remove`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）、`parameter_name`；可选：`emitter_version`、`save_after_set`
- `niagara_emitter_parameter_get`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）、`parameter_name`；可选：`emitter_version`
- `niagara_emitter_parameter_set`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）、`parameter_name`；并至少提供其一：`value_text`（或 `default_value_text`）/ `new_parameter_name` / `parameter_type`；可选：`emitter_version`、`save_after_set`
- `niagara_emitter_list_stages`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）；可选：`emitter_version`、`include_modules`、`include_module_inputs`
- `niagara_emitter_add_simulation_stage`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）；可选：`emitter_version`、`stage_class`、`stage_name`、`target_index`、`save_after_set`
- `niagara_emitter_remove_stage`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）+ `stage_key`（或 `script_usage` + `script_usage_id`）；可选：`emitter_version`、`save_after_set`
- `niagara_emitter_set_stage_property`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）+ `stage_key`（或 `script_usage` + `script_usage_id`）+ `property_name` + `value_text`；可选：`emitter_version`、`save_after_set`
- `niagara_emitter_list_stage_modules`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）+ `stage_key`（或 `script_usage` + `script_usage_id`）；可选：`emitter_version`、`include_inputs`
- `niagara_emitter_add_stage_module`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）+ `stage_key`（或 `script_usage` + `script_usage_id`）+ `module_script_asset_path`；可选：`emitter_version`、`target_index`、`module_name`、`save_after_set`
  - 当前行为：当目标 stage 模块栈为空时，会先自动补齐该 stage 的基础图结构，再执行加模块
- `niagara_emitter_remove_stage_module`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）+ `module_node_guid`；或 `stage_key`（或 `script_usage` + `script_usage_id`）+ `module_index` / `module_name`；可选：`emitter_version`、`save_after_set`
  - 当前行为：为避免破坏 Niagara Stack 图导致崩溃，执行“软删除”（即将模块设为 disabled），不会物理销毁节点
- `niagara_emitter_move_stage_module`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）+ `stage_key`（或 `script_usage` + `script_usage_id`）+ 模块标识（`module_node_guid` / `module_index` / `module_name`）+ `target_index`
  - 可选：`emitter_version`、`save_after_set`
  - 当前行为：仅重排当前 stage 的模块栈顺序，不改模块输入与 enabled 状态
- `niagara_emitter_list_stage_nodes`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）+ `stage_key`（或 `script_usage` + `script_usage_id`）；可选：`emitter_version`、`include_properties`、`include_module_inputs`
- `niagara_emitter_get_stage_node_property`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）+ `stage_key`（或 `script_usage` + `script_usage_id`）+ `node_guid` + `property_name`；可选：`emitter_version`
- `niagara_emitter_set_stage_node_property`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）+ `stage_key`（或 `script_usage` + `script_usage_id`）+ `node_guid` + `property_name` + `value_text`；可选：`emitter_version`、`save_after_set`
- `niagara_emitter_remove_stage_node`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）+ `stage_key`（或 `script_usage` + `script_usage_id`）+ `node_guid`；可选：`emitter_version`、`save_after_set`
  - 当前行为：若目标节点是模块节点，则执行“软删除”（disabled）；非模块节点仍执行物理删除
- `niagara_emitter_set_stage_module_enabled`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）+ `module_node_guid` + `enabled`；可选：`emitter_version`、`save_after_set`
- `niagara_emitter_list_module_inputs`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）+ `module_node_guid`；可选：`emitter_version`
  - 当前行为：会同时返回可见输入与隐藏的 Stack 输入（通过 `has_visible_pin` 区分）
- `niagara_emitter_set_module_input`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）+ `module_node_guid` + `input_name`；可选：`value_text`、`link_parameter_name`（或 `link_parameter`）、`emitter_version`、`break_input_links`、`save_after_set`
  - 链接参数：支持 `link_parameter_name`（或 `link_parameter`）将输入绑定到现有 Niagara 参数（例如 `Particles.HasCollided`）；也支持 `value_text="link:Particles.HasCollided"` 简写
  - 说明：当提供 `link_parameter_name`/`link_parameter` 时，`value_text` 可省略
  - 限制：若目标输入是 Static Switch（函数调用节点上的开关 pin），不支持 `link_parameter_name`，会返回 `input_link_not_supported_for_static_switch`
- `niagara_emitter_clear_module_input`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）+ `module_node_guid` + `input_name`；可选：`emitter_version`、`save_after_set`
  - 当前行为：会移除该输入对应的 override pin / linked value，恢复模块默认输入；若输入是 Static Switch，则仅重置默认值，不删除节点 pin
- `niagara_emitter_add_stage_node`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）+ `stage_key`（或 `script_usage` + `script_usage_id`）+（`module_script_asset_path` 或 `node_class`）；可选：`emitter_version`、`target_index`、`module_name`、`node_pos_x`、`node_pos_y`、`save_after_set`
  - 当前行为（防崩保护）：
    - 当通过 `module_script_asset_path` 向 `ParticleEventScript` stage 加模块时，直接拒绝并返回 `stage_add_stage_node_particle_event_script_blocked`
    - 当目标 stage 模块栈为空时，拒绝并返回 `stage_stack_invalid_or_empty_add_node_blocked`
- `niagara_emitter_connect_stage_nodes`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）+ `stage_key`（或 `script_usage` + `script_usage_id`）+ `from_node_guid`、`from_pin`、`to_node_guid`、`to_pin`；可选：`emitter_version`、`break_input_links`、`save_after_set`
- `niagara_emitter_disconnect_stage_nodes`
  - 关键参数：`emitter_asset_path`（或 `asset_path`）+ `stage_key`（或 `script_usage` + `script_usage_id`）+ `from_node_guid`、`from_pin`；可选：`to_node_guid`、`to_pin`、`emitter_version`、`save_after_set`

### 4.15 Control Rig / Control Rig Shape Library

Control Rig authoring 主流程使用文件夹式结构化 JSON。Shape Library 是独立资产，使用单文件 JSON；AnimBlueprint / Sequencer 接入和 bake 是跨资产动作，不由 `control_rig_apply_folder` 隐式完成。

- `control_rig_create`
  - 关键参数：`asset_path`
  - 可选参数：`control_rig_type=independent_rig|rig_module|modular_rig`、`preview_skeletal_mesh`、`import_hierarchy_from_preview`、`save_after_create`
- `control_rig_get_info`
  - 关键参数：`asset_path`
  - 返回：`control_rig_class`、`preview_skeletal_mesh`、`shape_libraries[]`、`hierarchy`、`graphs[]`、`variables`
- `control_rig_export_folder`
  - 关键参数：`asset_path`
  - 可选参数：`folder_path`、`clean_output_dir`
  - 说明：导出 `asset/settings/shape_libraries/hierarchy/variables/graphs/modular/validation` 文件夹结构，并写入 `validation/coverage_report.json`
- `control_rig_validate_folder`
  - 关键参数：`folder_path`
  - 可选参数：`asset_path`、`create_if_missing`
  - 返回：`valid`、`json_issue_count`、`error_count`、`warning_count`、`issues[]`、`coverage`
- `control_rig_apply_folder`
  - 关键参数：`folder_path`
  - 可选参数：`asset_path`、`dry_run`、`validate_only`、`create_if_missing`、`compile_after_apply`、`save_after_apply`
  - 返回：`valid`、`issues[]`、`applied.variables_added`、`compile_report`、`readback`
  - 说明：当前稳定回写 preview、Shape Library 引用、hierarchy bones/nulls/controls/curves、`variables/variables.json` 和 `graphs/graphs.json`。`functions`、`modular`、`raw_properties`、`readonly_properties` 等 profile 如果包含实际写入内容，会失败并返回 `unsupported_apply_profile`，不静默忽略
- `control_rig_compile`
  - 关键参数：`asset_path`
- `control_rig_get_compile_log`
  - 关键参数：`asset_path`
- `control_rig_open_editor`
  - 关键参数：`asset_path`
  - 说明：只用于调试辅助，必须遵守最小化/不抢焦点规则
- `control_rig_graph_get_view / control_rig_graph_set_view`
  - 关键参数：`asset_path`
  - 可选参数：`graph_name_or_path`
- `control_rig_viewport_get_camera / control_rig_viewport_set_camera`
  - 关键参数：`asset_path`
- `control_rig_screenshot`
  - 关键参数：`asset_path`、`target=graph|viewport|window`
  - 可选参数：`graph_name_or_path`
- `control_rig_runtime_probe`
  - 关键参数：`asset_path`
  - 可选参数：`frames`、`event_name`、`execute_construction`、`variables`、`variable_inputs[]`、`sample_bones[]`、`sample_controls[]`、`sample_variables[]`
  - 返回：`supported_events`、`construction_*`、`execution_*`、`compile_report`、`issues[]`、`bones[]`、`controls[]`、`variables[]`
- `control_rig_bake_to_animation`
  - 关键参数：`sequence_path`、`binding_id` 或 `binding_guid`、`output_anim_sequence`
  - 新建 AnimSequence 时优先从 binding 上的 SkeletalMesh 推断 skeleton，也可显式传 `target_skeleton` 或 `preview_skeletal_mesh`
  - 返回：`binding_preflight`、`export_api`、`created`、`exported`
- `control_rig_bake_to_control_rig`
  - 关键参数：`sequence_path`、`binding_id` 或 `binding_guid`、`control_rig_class`
  - 目标类必须是 `UFKControlRig` 或支持 Inverse event；否则返回 `control_rig_bake_requires_fk_or_inverse_event`
  - 返回：`binding_preflight`、`is_fk_control_rig`、`supports_inverse_event`、`baked`
- `control_rig_shape_library_create`
  - 关键参数：`asset_path`
- `control_rig_shape_library_get_info`
  - 关键参数：`asset_path`
- `control_rig_shape_library_export_json`
  - 关键参数：`asset_path`
  - 可选参数：`output_file`
- `control_rig_shape_library_validate_json`
  - 关键参数：`json_file`
  - 可选参数：`asset_path`
- `control_rig_shape_library_apply_json`
  - 关键参数：`json_file`
  - 可选参数：`asset_path`、`create_if_missing`、`save_after_apply`

推荐验收：`control_rig_validate_folder` 必须 `json_issue_count=0`；`control_rig_apply_folder` 必须 `compile_report.error_count=0`；变量或 graph 写入必须在 `readback` 中读到；运行逻辑用 `control_rig_runtime_probe` 验证输入变量和采样结果。完整字段见 `commands/17_ControlRig_FolderFormat.md`。

## 5. PowerShell 模板

```powershell
$base = "http://127.0.0.1:17777"
$token = "<your_token>"
$h = @{ "X-UeAgentInterface-Token" = $token }

function Invoke-UAI($id, $cmd, $params) {
  $body = @{
    request_id = $id
    command    = $cmd
    params     = $params
  } | ConvertTo-Json -Depth 20

  Invoke-RestMethod -Method Post `
    -Uri "$base/api/exec" `
    -Headers $h `
    -ContentType "application/json" `
    -Body $body
}
```

## 6. 脏资源处理与安全关闭编辑器

推荐新流程：

1. `editor_list_dirty_resources` 先枚举所有待处理脏资源
2. `editor_resolve_dirty_resources` 按路径或整批保存/丢弃
3. `editor_close` 真正请求关闭；如果还有未处理脏资源，会失败并返回完整清单

### 6.1 `editor_list_dirty_resources`

- `command`: `editor_list_dirty_resources`
- 无参数

返回：
- `dirty_resource_count`
- `dirty_resources[]`
  - `resource_path`
  - `object_path`
  - `kind`（`level / asset`）
  - `is_current_level`
  - `is_open_in_editor`
  - `decision_required`

### 6.2 `editor_resolve_dirty_resources`

- `command`: `editor_resolve_dirty_resources`
- `params`：
  - `save_resource_paths`
  - `discard_resource_paths`
  - 兼容旧字段：`save_asset_paths` / `discard_asset_paths`
  - 兼容别名：`save_resources` / `discard_resources`
  - `save_current_level`
  - `discard_current_level`
  - `save_all_dirty`
  - `discard_all_dirty`
  - 兼容别名：`save_all` / `discard_all`
  - `close_all_asset_editors`
  - `only_save_dirty`

返回：
- `dirty_resources_before`
- `dirty_resources_after`
- `remaining_dirty_resource_count`
- `all_dirty_resources_resolved`

### 6.3 `editor_close`

- `command`: `editor_close`
- `params`：
  - `request_exit`：是否真正退出，默认 `true`
  - `close_all_asset_editors`：退出前是否关闭全部资产编辑器，默认 `true`

行为：
- 若还有未处理脏资源，会失败并返回 `editor_has_unresolved_dirty_resources`
- 同时在 `data.dirty_resources` 里给出完整清单
- 只有脏资源全部处理完后，才会允许关闭

### 6.4 兼容命令：`editor_prepare_exit`

使用 `editor_prepare_exit` 可以在一次调用里完成“保存/丢弃 + 请求退出”。它仍然可用，但推荐新脚本优先拆成上面三步，便于在关闭失败时拿到更明确的脏资源列表。

- `command`: `editor_prepare_exit`
- `params`：
  - `save_asset_paths`：要保存的资产路径数组（如 `["/Game/AutoTests/Niagara/NS_Test"]`）
  - `discard_asset_paths`：要丢弃改动的资产路径数组
  - 兼容别名：`save_resources` / `discard_resources`
  - `save_current_level`：是否保存当前关卡（默认 `false`）
  - `discard_current_level`：是否丢弃当前关卡改动（默认 `false`）
  - `save_all_dirty`：是否额外保存所有脏包（默认 `false`）
  - `discard_all_dirty`：是否自动丢弃剩余脏包，避免退出弹窗（默认 `true`）
  - 兼容别名：`save_all` / `discard_all`
  - `close_all_asset_editors`：退出前关闭资产编辑器（默认 `true`）
  - `request_exit`：是否发起退出（默认 `true`）
  - `only_save_dirty`：保存时仅保存脏包（默认 `true`）

示例：

```json
{
  "request_id": "exit-001",
  "command": "editor_prepare_exit",
  "params": {
    "save_asset_paths": [
      "/Game/AutoTests/Niagara/NS_BulletTrailHitExplosion_20260224_142032"
    ],
    "discard_asset_paths": [
      "/Game/AutoTests/Niagara/NS_TempDraft"
    ],
    "save_current_level": false,
    "discard_current_level": true,
    "discard_all_dirty": true,
    "request_exit": true
  }
}
```


### 2026-04-22 增补

- Blueprint 图施工补充：
  - `blueprint_add_enhanced_input_action_event`
  - `blueprint_add_dynamic_cast_node`
  - `blueprint_add_enhanced_input_local_player_subsystem_node`
  - `blueprint_add_enhanced_input_add_mapping_context_node`
- `WidgetBlueprint` 的 `settings/widget_blueprint.json` 新增 `is_focusable`，`umg_apply_folder` 会同步回写 `bIsFocusable`。
