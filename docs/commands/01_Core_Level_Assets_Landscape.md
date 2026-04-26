# 指令详解：Core / Level / Assets / Landscape

## Core / Session

| 指令 | 作用 | 关键参数 | 典型用途 |
|---|---|---|---|
| `get_world_state` | 读取当前编辑器世界状态 | 无 | 批处理前确认当前关卡和世界是否有效 |
| `begin_transaction` | 开启 UE 事务 | `label` | 多步修改前包一层可撤销事务 |
| `end_transaction` | 结束 UE 事务 | `commit` | 成功提交；失败时可不提交 |
| `undo` | 撤销上一步事务 | 无 | 快速回滚错误修改 |
| `redo` | 重做已撤销事务 | 无 | 验证回滚链路 |
| `exec_batch` | 顺序执行多条命令，支持遇错中断 | `commands[]`、`stop_on_error` | 复杂自动化的默认入口 |
| `save_current_level` | 保存当前关卡 | `only_if_dirty` | 批处理结束后落盘 |
| `editor_get_open_assets` | 列出当前已打开的资产编辑器 | 无 | 获取编辑器上下文 |
| `open_asset_editor` | 打开指定资产编辑器 | `asset_path` | 打开 Blueprint / Material / Niagara |
| `save_asset` | 保存指定资产 | `asset_path`、`only_if_dirty` | 保存关键资产修改 |
| `asset_duplicate` | 复制资产到指定路径 | `source_asset_path`、`destination_asset_path`、`save_after_duplicate` | 先复制测试资产，再在副本上做安全修改 |
| `asset_import_fbx_skeletal_mesh` | 导入 FBX 为 Skeletal Mesh | `source_filename`、`destination_path`，可选 `skeleton_path/import_materials/import_textures/create_physics_asset/import_animations` | 把外部角色模型/骨骼导入到项目 |
| `asset_import_fbx_animation` | 导入 FBX 为 AnimSequence | `source_filename`、`destination_path`，以及 `skeleton_path` 或 `skeletal_mesh_path` | 批量导入第三方动作到现有骨骼 |
| `asset_export_property_json` | 导出资产属性为 JSON | `asset_path`；可选 `property_names[]`、`output_file` | 把 AnimSequence / Texture / Mesh 的常用属性拉成可编辑 JSON |
| `asset_apply_property_json` | 从 JSON 回写资产属性 | `asset_path` 或 `json_file`；可选 `properties[]`、`save_after_apply` | 按 JSON 批量回写 AnimSequence / Texture / Mesh 的属性 |
| `editor_list_dirty_resources` | 列出当前所有待处理的脏资源 | 无 | 退出前先枚举未保存关卡/资产 |
| `editor_resolve_dirty_resources` | 按路径或整批保存/丢弃脏资源 | `save_resource_paths`、`discard_resource_paths`、`save_all_dirty`、`discard_all_dirty` | 先处理脏资源，再决定是否关闭编辑器 |
| `editor_close` | 关闭编辑器；若仍有未处理脏资源则失败并返回清单 | `request_exit`、`close_all_asset_editors` | 自动化结束时安全退出 |
| `editor_prepare_exit` | 退出前按策略保存/丢弃并请求关闭编辑器 | `save_asset_paths`、`discard_asset_paths`、`discard_all_dirty`、`request_exit` | 自动化结束时避免恢复弹窗 |

## Level / Actor / Component

| 指令 | 作用 | 关键参数 | 典型用途 |
|---|---|---|---|
| `list_actors` | 列出当前关卡 Actor | `limit` | 读取场景现有对象 |
| `spawn_actor` | 生成 Actor | `class_path`、可选 `label/location/rotation/scale/static_mesh/folder_path/tags[]/snap_to_ground` | 白盒搭建、生成测试对象 |
| `level_spawn_wall_with_opening` | 生成“带开口的墙”（自动拆成多段网格） | `plane`、`wall_size`、`opening_center/opening_size`、`label_prefix` | 门洞/窗洞/通道口：避免手工拆墙造成接口不连通 |
| `destroy_actor` | 删除 Actor | `id` | 清理测试对象 |
| `level_mark_probe` | 创建验收探针（TargetPoint） | `location`、可选 `label/folder_path/tags` | 固化关键连接处验收点位 |
| `level_generate_probes` | 批量创建验收探针（TargetPoint） | `points[]` 或 `probes[]`、可选 `label_prefix/folder_path/tags` | 一次生成多组 probe 便于回归 |
| `level_duplicate_actor` | 复制关卡 Actor | `id`、`offset` | 快速铺设模块化建筑件 |
| `actor_list_components` | 列出 Actor 组件 | `id`、`include_non_scene` | 修改组件前定位组件名 |
| `level_list_actor_components` | `actor_list_components` 的同义命令 | 同上 | 统一走 `level_*` 前缀 |
| `level_get_actor_property` | 读取 Actor 属性 | `id`、`property_name` | 调试实例状态 |
| `level_get_component_property` | 读取组件属性 | `id`、`component`（或 `component_id`）、`property_name` | 查询 Mesh / Collision / Material 槽 |
| `actor_set_property` | 设置 Actor 属性 | `id`、`property_name`、`value_text` | 修改显示、碰撞、行为参数 |
| `level_set_actor_property` | `actor_set_property` 的同义命令 | 同上 | 统一走 `level_*` 前缀 |
| `component_set_property` | 设置组件属性 | `id`、`component`（或 `component_id`）、`property_name`、`value_text` | 修改组件模板值 |
| `level_set_component_property` | `component_set_property` 的同义命令 | 同上 | 统一走 `level_*` 前缀 |
| `level_get_actor_transform` | 读取 Actor 实例变换 | `id` | 精确读取 `location / rotation / scale` |
| `level_set_actor_transform` | 一次性写入实例变换（可选碰撞感知放置） | `id`，可选 `location`、`rotation`、`scale`、`collision_aware`、`collision{...}` | 原子修改实例变换，减少穿插/卡住 |
| `level_set_actor_location` | 仅写位置 | `id`、`location` | 快速重排对象 |
| `level_set_actor_rotation` | 仅写旋转 | `id`、`rotation` | 统一朝向 |
| `level_set_actor_scale` | 仅写缩放 | `id`、`scale` | 微调模块尺寸 |
| `level_attach_actor` | 把子 Actor 附加到父 Actor | `child_id`、`parent_id` | 建立层级关系 |
| `level_detach_actor` | 分离 Actor | `id` | 拆开层级 |
| `level_set_actor_folder` | 设置 Outliner 文件夹 | `id`、`folder_path` | 整理关卡层级 |
| `level_destroy_folder_actors` | 按 Outliner 文件夹批量删除 Actor | `folder_path`、`include_child_folders` | 迭代白盒前原子清空整组结构，避免残留重叠 |
| `level_cleanup_empty_folders` | 清理未被任何 Actor 使用的空 folder | `folder_path_prefix`、`dry_run` | 清场后清理残留空文件夹，保持 Outliner 整洁 |
| `level_add_actor_tag` | 增加 Actor Tag | `id`、`tag` | 供后续脚本筛选和标记 |
| `level_get_selection` | 读取当前选择集 | 无 | 获取当前选中对象 |
| `level_set_selection` | 设置选择集 | `actor_ids`、`mode` | 后续聚焦、批量操作 |
| `navmesh_build` | 构建/刷新 NavMesh | `wait_for_finish`、`timeout_seconds` | 白盒验收前生成可查询导航数据 |
| `navmesh_project_point` | 单点投影到 NavMesh（只投影不查路） | `point`（或 `location`）、`project_query_extent` | 查路前先确认“投影落点是否在期望层面” |
| `navmesh_find_path` | NavMesh 两点查路 | `start`、`end`、`allow_partial`、`project_to_nav` | 批量判定关键节点连通性 |
| `navmesh_spawn_bounds_volume` | 创建 NavMeshBoundsVolume（带有效 Brush Bounds） | `bounds`、可选 `rotation/label/folder_path` | 自动化白盒：确保 NavMesh 可构建、可投影、可查路 |
| `level_validate_connectivity` | 多点连通性验收（walk + 显式边） | `points[]` 或 `probe_actor_ids[]`、可选 `pairs[].edge_type`、`graph_root_index` | 把“断路点/断回环”结构化输出为逐对+图报告 |
| `level_trace_world_ray` | 世界射线 Trace（不依赖 viewport） | `start`、`direction`、`trace_distance`、`trace_channel` | 查地面、查遮挡、查通道是否堵住 |
| `level_snap_to_surface` | 沿射线吸附 Actor 到命中面 | `id`、可选 `start/direction/offset_cm` | 快速贴合地面/墙面，减少悬空与缝隙 |
| `level_sweep_capsule` | 胶囊 Sweep 通行性检测 | `start`、`end`、`radius_cm`、`half_height_cm`、`trace_channel` | 检查门洞/走廊/电梯轨迹净空 |
| `level_sweep_capsule_path` | 折线路径胶囊 Sweep（路径净空） | `points[]`、`radius_cm`、`half_height_cm`、`step_cm` | 检查楼梯/走廊/电梯轨迹中途阻挡与净空不足 |
| `level_check_overlaps` | Overlap 检测（碰撞穿插） | `shape`、`center`、形状参数、`trace_channel` | 发现“摆放穿插/互相卡住/门墙不贴合”等问题 |

### 实例变换相关建议

- 批量脚本中，优先使用 `level_get_actor_transform` / `level_set_actor_transform`
- 不建议再绕用通用 `property_name=RelativeLocation` 之类写法做实例位姿编辑
- 需要单独改某一维度时再用 `level_set_actor_location / rotation / scale`
- 生成/对齐后容易出现“穿插/卡住/留缝”时：优先用 `level_set_actor_transform(collision_aware=true)` 做放置前的 Overlap 检测（必要时开启 `collision.auto_fallback` 多候选偏移），再配合 `level_check_overlaps` 做局部复核。

### `level_set_actor_transform`

一次性写入 Actor 实例变换；在白盒迭代中常用来做“局部精准修复”。

- 必填：`id`
- 可选：`location`、`rotation`、`scale`
- 可选：`collision_aware`（默认 `false`）
  - 仅在本次写入包含 `location` 时生效
  - 为 `true` 时会在写入前做一次（或多次候选）Overlap 检测，找到满足条件的位置后再落地写入；若所有候选都失败，会返回 `ok=false` 且 `error=collision_aware_no_valid_location`，同时仍会回传 `collision_result.attempts[]` 便于诊断

`collision{...}`（可选，仅 `collision_aware=true` 时读取）：

- `trace_channel`：可选，默认 `Visibility`
- `trace_complex`：可选，默认 `false`
- `max_allowed_overlaps`：可选，默认 `0`
- `shape`：可选，`box/sphere/capsule`
  - 不填时默认使用 Actor 当前 bounds 的 world AABB 作为 box（`shape_derived_from_bounds=true`）
  - `shape=box`：必填 `box_extent`
  - `shape=sphere`：必填 `radius_cm`
  - `shape=capsule`：必填 `radius_cm`、`half_height_cm`，可选 `rotation`
  - `bounds_padding_cm` / `padding_cm`：仅在“从 bounds 派生 shape”时生效；对派生 box 的 extent 做等量扩/缩（cm，可为负）
- `ignore_actor_ids[]`：可选，忽略的 Actor 名称/标签数组（会自动忽略目标 Actor 自身）
- `ignore_folder_path_prefix` / `ignore_tags[]` / `ignore_class_substrings[]`：可选，忽略过滤器（同 `level_trace_world_ray`），用于屏蔽噪声对象
- `include_overlaps`：可选，默认 `false`；为 `true` 时返回每次尝试的 overlaps 列表（受 `overlap_limit` 限制）
- `overlap_limit`：可选，默认 `10`
- `auto_fallback`：可选，默认 `true`；在目标位置重叠时尝试偏移候选
- `fallback_step_cm`：可选，默认 `10`
- `fallback_offsets_cm[]`：可选，显式候选偏移（每项为向量 `{"x","y","z"}` 或对象 `{"offset_cm":{...}}`）；提供后将按列表顺序尝试

返回：除 `location/rotation/scale/transform` 外，还会包含 `collision_result`（记录请求位置、最终位置、候选尝试与重叠统计）。

### `level_spawn_wall_with_opening`

用途：一次性生成“墙面矩形 - 开口矩形”的剩余部分（最多 4 段）。典型用来做门洞/通道口，避免手工拆成多段墙时遗漏接口验收导致的“视觉连着但实际被墙封死”。

关键参数：

- `plane`：墙所在平面（用 `center/normal/up` 定义局部坐标系；厚度轴=normal，宽度轴=right，高度轴=up）
  - `center`：墙中心点（世界坐标，cm）
  - `normal`：墙法线方向（世界向量，单位不要求；会归一化）
  - `up`：可选，默认 `{0,0,1}`（若与 normal 近似共线会自动换一个 up）
- `wall_size`：墙尺寸（cm）
  - `thickness_cm`：厚度
  - `width_cm`：宽度（沿 right）
  - `height_cm`：高度（沿 up）
- `opening_center`：开口中心相对墙中心的偏移（cm，位于墙平面内）
  - `right_cm`：沿 right 偏移
  - `up_cm`：沿 up 偏移
- `opening_size`：开口尺寸（cm）
  - `width_cm`：沿 right
  - `height_cm`：沿 up
- `label_prefix`：生成段的标签前缀（实际会生成 `_Left/_Right/_Top/_Bottom` 后缀）
- `folder_path`：可选，Outliner folder

可选参数：

- `opening_padding_cm`：开口扩边（双向各加 padding），用于给门洞留净空
- `clamp_opening`：开口超出墙边界时是否自动夹紧（默认 `false`，建议在调试阶段保持严格失败）
- `min_segment_size_cm`：小于该尺寸的段会跳过生成（默认 `1.0`）
- `class_path`：可选，生成段使用的 Actor 类（默认 `/Script/Engine.StaticMeshActor`）
- `static_mesh`：可选，生成段使用的 StaticMesh（默认 `/Engine/BasicShapes/Cube.Cube`）
- `epsilon_cm`：可选，边界判定用的容差（默认 `0.01`），通常无需调整

示例：生成一面 Y 向墙（normal=(1,0,0)），底边贴地、带门洞（门洞宽 140、高 220，底到地面）：

```json
{
  "command": "level_spawn_wall_with_opening",
  "params": {
    "plane": { "center": { "x": -3220, "y": 1250, "z": 550 }, "normal": { "x": 1, "y": 0, "z": 0 }, "up": { "x": 0, "y": 0, "z": 1 } },
    "wall_size": { "thickness_cm": 40, "width_cm": 1400, "height_cm": 300 },
    "opening_center": { "right_cm": 0, "up_cm": -30 },
    "opening_size": { "width_cm": 140, "height_cm": 220 },
    "opening_padding_cm": 10,
    "label_prefix": "WB_WallW_Door",
    "folder_path": "UAIValidation/Walls"
  }
}
```

### `level_mark_probe`

创建一个 `TargetPoint` 作为“验收探针/锚点”（建议把探针语义固定为 **脚底点/落脚点**，便于后续 Trace/Sweep/连通性验收复用）。

- `location`：必填，世界坐标。
- `rotation`：可选，默认 `0,0,0`。
- `label`：可选，Actor Label。
- `folder_path`：可选，Outliner folder（推荐统一放 `.../Probes`）。
- `tags[]`：可选，Actor Tags。

返回：`probe`（完整 actor_info）以及 `location/rotation`。

### `level_generate_probes`

批量创建 `TargetPoint` 探针（用于回归、批量连通性验收、关键落脚点固化）。

- `points[]`：vector 列表（每个元素是 `{x,y,z}`），或
- `probes[]`：对象列表（每个元素至少包含 `location`），二选一必填
- 可选：
  - `label_prefix`（默认 `Probe`，会自动生成 `Probe_01/02/...`）
  - `folder_path`
  - `rotation`
  - `tags[]`

返回：`count` 与 `probes[]`（actor_info 列表）。

## Viewport / 屏幕点 / 对齐

| 指令 | 作用 | 关键参数 | 典型用途 |
|---|---|---|---|
| `viewport_get_camera` | 读取当前视口相机 | 无 | 保存当前构图 |
| `viewport_set_camera` | 设置视口相机 | `location`、`rotation`、`fov` | 快速移动到目标区域 |
| `viewport_set_realtime` | 开关实时渲染 | `realtime`、`store_current_value` | Niagara / 动画检查前开启实时 |
| `viewport_set_game_view` | 开关 Game View | `game_view` | 截图前隐藏编辑器辅助元素 |
| `viewport_focus_actor` | 聚焦指定 Actor | `id` | 快速定位对象 |
| `viewport_focus_actor_safe` | 更稳的聚焦：默认防卡墙 + 自动看向目标 | `id`，可选透传 `padding/collision_aware/look_at/...` | 室内白盒验收取景，减少相机卡墙/朝向不对 |
| `viewport_frame_actor` | 按单个 Actor bounds 仅平移相机入画 | `id`、`padding` | 保持当前旋转和 FOV，快速查看单个对象 |
| `viewport_frame_selection` | 聚焦当前选择集 | `instant` | 验收当前选中结果 |
| `viewport_frame_actors` | 聚焦指定 Actor 列表 | `actor_ids`、`instant` | 从脚本直接构图 |
| `viewport_frame_folder` | 按 folder 聚合 Actor bounds 并仅平移相机入画 | `folder_path`、`include_child_folders`、`padding` | 保持当前旋转和 FOV，快速查看整组结构 |
| `viewport_deproject_screen_to_world` | 屏幕点反投影 | `screen_x`、`screen_y` | 从像素点得到世界射线 |
| `viewport_trace_screen_point` | 从屏幕点发起 Trace | `screen_x`、`screen_y`、`trace_distance`、`trace_channel`、`trace_complex` | 获取命中位置和法线 |
| `viewport_pick_actor_at_screen` | 从屏幕点拾取命中的 Actor 并返回 Actor 信息 | `screen_x`、`screen_y`、`trace_distance`、`trace_channel`、`trace_complex` | 点击式检查场景对象 |
| `viewport_select_actor_at_screen` | 从屏幕点拾取并设置当前选择，同时返回完整 Actor 信息 | `screen_x`、`screen_y`、`selection_mode`、`trace_distance` | 点击式选择并检查对象 |
| `level_get_nearby_actor_obbs` | 读取某个 Actor 周围球形范围内 Actor 的 OBB 信息 | `id`、`radius`、`include_self` | 白盒空间检查、碰撞邻近审计 |
| `screenshot_viewport` | 抓取当前视口截图 | `format`、`quality`、`max_size` | 自动化留证 |
| `screenshot_viewport_buffer` | 抓取当前视口深度/法线/底色等 buffer 截图（默认 SceneDepth） | `buffer`、`format`、`quality`、`max_size` | 用深度图等调试视图定位空间错误 |
| `mesh_get_closest_vertex` | 查询最近顶点 | `id`，可选 `component`、`world_point` | 顶点辅助对齐 |
| `mesh_get_vertex_world_position` | 读取顶点世界坐标 | `id`，可选 `component`、`vertex_index` | 复核顶点位置 |
| `level_align_actor_vertex_to_vertex` | 顶点到顶点实例对齐 | `source_actor_id`、`target_actor_id`、源/目标顶点索引或世界点 | 模块拼装、几何校验 |
| `level_align_actor_by_bounds` | 按包围盒锚点对齐 | `source_actor_id`、`target_actor_id`、`axis`、`source_anchor`、`target_anchor`、`offset` | 建筑模块、平台、楼板拼装 |
| `level_align_face_to_face` | 面对面贴合（基于 bounds） | `source_actor_id/target_actor_id`、`source_face/target_face`、可选 `axis/offset_cm` | 门/墙贴合、地面/电梯贴合、补齐缝隙 |

### `level_align_actor_by_bounds`

- `axis`：`x / y / z`
- `source_anchor` / `target_anchor`：`min / center / max`
- `offset`：在对齐结果上追加偏移量

常见用法：

- `source=min` 对 `target=max`：把一块墙贴到另一块墙外侧
- `center` 对 `center`：沿某轴居中
- `max` 对 `max`：把两个包围盒同一侧对齐

相比 `level_align_actor_vertex_to_vertex`，`level_align_actor_by_bounds` 更适合白盒建筑和模块化关卡拼装。

### `level_align_face_to_face`

`level_align_actor_by_bounds` 的白盒友好封装：用“面语义”表达对齐（门/墙、电梯/地面、板/板贴合），避免每次手写 `axis + anchor`。

- `source_face` / `target_face`：
  - `min/max/center`：沿 `axis` 使用对应锚点
  - 或显式：`+x/-x/+y/-y/+z/-z`（会推导 `axis`）
- `offset_cm`：可选，对齐后再加一个间隙/嵌入偏移（cm）

### `viewport_frame_folder`

- `folder_path`：目标 Outliner folder 路径。
- `include_child_folders`：是否递归包含子 folder，默认 `true`。
- `padding`：额外取景留白倍数，默认 `1.1`。
- `collision_aware`：可选，是否启用“室内取景防卡墙”，默认 `false`。
- `safety_offset_cm`：可选，启用防卡墙时使用的安全偏移（cm），默认 `15.0`。
- `trace_channel`：可选，防卡墙 Trace 通道名，默认 `Visibility`。
- `trace_complex`：可选，防卡墙是否使用复杂碰撞，默认 `false`。
- `look_at`：可选，是否把相机朝向自动对准 bounds center，默认 `false`。
- `auto_fallback`：可选，防卡墙增强：自动尝试多个候选相机位，默认 `false`。
- `fallback_step_cm`：可选，`auto_fallback=true` 时的偏移步长（cm），默认 `200`。
- `fallback_offsets_cm`：可选，自定义候选偏移（世界坐标 cm 向量数组）；当提供时，`auto_fallback` 的默认候选不会自动生成。

行为说明：

- 会遍历 folder 下 Actor，聚合有效 bounds。
- 默认只重新计算并写入相机 `location`；若 `look_at=true` 会同步修改 `rotation`（`roll=0`）。
- 返回 `desired_location`（基准理想位）与 `new_location`（实际写入），并附带 `rotation/fov`。
- 当 `collision_aware=true` 时，会从目标 bounds center 向候选相机位做 Trace：
  - 未命中遮挡：直接使用候选位。
  - 命中遮挡：把相机放到命中点沿“Trace 方向反向”回退 `safety_offset_cm` 的位置，避免穿墙/卡墙。
  - 若 `auto_fallback=true` 或提供 `fallback_offsets_cm`，会在多候选中选择“距离目标更远且无遮挡/可修正”的相机位，并返回 `fallback_used/fallback_index/fallback_offset_cm`。
- 仅支持透视视口；若当前有效视口不是透视视口，会返回错误。

### `screenshot_viewport_buffer`

抓取当前视口的“深度/法线/底色”等调试截图（默认深度图），用于更可靠地发现白盒空间错误：穿插、悬空、遮挡、缝隙、楼梯被挡等。

- `buffer`：可选，默认 `SceneDepth`。常用：
  - `SceneDepth`
  - `DeviceDepth`
  - `WorldNormal`
  - `BaseColor`

说明：`SceneDepth/DeviceDepth/WorldNormal/BaseColor` 会优先走 **SceneCapture**（不依赖 editor debug viewmode shader，稳定）；其他值会回退到 **Buffer Visualization**（可能受引擎/项目设置影响）。

其他参数与 `screenshot_viewport` 一致：
- `format`：`png/jpg/webp`
- `quality`：1~100（对 `png` 无效）
- `max_size`：最大边长（会等比缩放）

深度图附加参数（仅 `SceneDepth/DeviceDepth` 生效）：
- `invert`：可选，默认 `true`；为 `true` 时“近亮远暗”（更像 UE 的深度调试观感）。
- `depth_mode`：可选，仅 `SceneDepth` 使用；默认 `auto_percentile`（当未提供 `depth_far_cm` 时）。可选：
  - `auto/auto_percentile/percentile`：从截图像素采样 depth，按分位数自动选取可视化范围（默认 2%~98%），避免视口相机较远时整张图接近纯黑/纯白。
  - `fixed/fixed_far/far`：固定范围映射（`depth_near_cm`~`depth_far_cm`）。
- `depth_near_cm`：可选，仅 `SceneDepth` 使用；固定模式下的近端（cm），默认 `0`。
- `depth_far_cm`：可选，仅 `SceneDepth` 使用；**提供后默认走 fixed 模式**。固定模式下默认 `10000`（未提供时）。
- `depth_auto_pct_low` / `depth_auto_pct_high`：可选，仅 auto 模式；默认 `2` / `98`（0~100）。

行为说明：

- **优先 SceneCapture（推荐）**：直接用当前 Level Viewport 的相机（位置/旋转/FOV）做一次 SceneCapture，并读取 RenderTarget 得到对应 buffer；不依赖 `Buffer Visualization` 的 debug 材质。
- **回退 Buffer Visualization（实验）**：临时切换当前 Level Viewport 到 `Buffer Visualization` 模式并设置 `<buffer>`；截图完成后恢复原 view mode 与原 Buffer Visualization Mode。
- 返回字段会带 `buffer` 与 `method`（`scene_capture` / `buffer_visualization`），便于批量留证与回归对比。
- 目标 viewport 选择：优先当前正在编辑的 Level Viewport；若无法获取“当前”，则选择面积最大的透视视口（避免抓到隐藏/未渲染视口导致纯黑截图）。

### `level_destroy_folder_actors`

- `folder_path`：目标 Outliner folder 路径。
- `include_child_folders`：是否递归包含子 folder，默认 `true`。

行为说明：

- 会遍历 folder 下 Actor，并按当前世界里的真实 folder 状态聚合目标。
- 目标集合为空时仍返回成功，`deleted_count=0`，适合做幂等清理。
- 内部会一次性调用编辑器批量删除，而不是要求调用方先枚举再逐个 `destroy_actor`。
- 适合在批量白盒重建前先清空上一次生成的整组对象，避免因 summary 漏项导致残留重叠。

### `level_cleanup_empty_folders`

- `folder_path_prefix`：可选，只清理该前缀下的空 folder（例如 `WB03_TowerStep`）。
- `dry_run`：可选，默认 `false`；为 `true` 时只返回将要删除的 folder 列表，不实际删除。

行为说明：

- 会枚举当前世界里的 Outliner folders（包括历史残留的空 folder）。
- 通过扫描当前世界里所有 Actor 的 `FolderPath`，推导“正在被使用的 folder 集合”（含父级路径）。
- 对未被任何 Actor 使用的 folder 调用删除（按深度从深到浅排序，避免父级先删导致失败）。
- 典型用法：先 `level_destroy_folder_actors` 清场，再 `level_cleanup_empty_folders` 清理残留空 folder。

### `viewport_frame_actor`

- `id`：目标 Actor 名称或标签。
- `padding`：额外取景留白倍数，默认 `1.1`。
- `collision_aware`：可选，是否启用“室内取景防卡墙”，默认 `false`。
- `safety_offset_cm`：可选，启用防卡墙时使用的安全偏移（cm），默认 `15.0`。
- `trace_channel`：可选，防卡墙 Trace 通道名，默认 `Visibility`。
- `trace_complex`：可选，防卡墙是否使用复杂碰撞，默认 `false`。
- `look_at`：可选，是否把相机朝向自动对准 bounds center，默认 `false`。
- `auto_fallback`：可选，防卡墙增强：自动尝试多个候选相机位，默认 `false`。
- `fallback_step_cm`：可选，`auto_fallback=true` 时的偏移步长（cm），默认 `200`。
- `fallback_offsets_cm`：可选，自定义候选偏移（世界坐标 cm 向量数组）；当提供时，`auto_fallback` 的默认候选不会自动生成。

行为说明：

- 使用目标 Actor 的有效 bounds。
- 默认只重新计算并写入相机 `location`；若 `look_at=true` 会同步修改 `rotation`（`roll=0`）。
- 返回 `desired_location`（基准理想位）与 `new_location`（实际写入），并附带 `rotation/fov`。
- 当 `collision_aware=true` 时，会从目标 bounds center 向候选相机位做 Trace：
  - 未命中遮挡：直接使用候选位。
  - 命中遮挡：把相机放到命中点沿“Trace 方向反向”回退 `safety_offset_cm` 的位置，避免穿墙/卡墙。
  - 若 `auto_fallback=true` 或提供 `fallback_offsets_cm`，会在多候选中选择“距离目标更远且无遮挡/可修正”的相机位，并返回 `fallback_used/fallback_index/fallback_offset_cm`。
- 仅支持透视视口；若当前有效视口不是透视视口，会返回错误。

### `viewport_focus_actor_safe`

用于白盒验收的“更稳聚焦”：

- 内部走 `viewport_frame_actor` 的逻辑（而不是 `FocusViewportOnBox`）
- 默认开启：
  - `collision_aware=true`（防卡墙）
  - `look_at=true`（自动朝向目标）
  - `auto_fallback=true`（多候选取景点）

### `viewport_pick_actor_at_screen`

- `screen_x` / `screen_y`：屏幕像素坐标。
- `trace_distance`：可选，Trace 最大距离，默认 `100000.0`。
- `trace_channel`：可选，碰撞通道名，默认 `Visibility`。
- `trace_complex`：可选，是否使用复杂碰撞，默认 `true`。
- `ignore_actor_ids`：可选，忽略的 Actor 名称或 Label 列表（用于避开遮挡物）。
- `allow_no_hit`：可选，默认 `false`；为 `true` 时未命中也返回成功并给出 `hit=false`。

行为说明：

- 先把屏幕点反投影成世界射线，再做一次直线 Trace。
- 成功时会返回命中 Actor 摘要、Component 名称与路径，以及命中位置、法线、距离等信息。
- 未命中 Actor 时：
  - `allow_no_hit=false`：返回错误 `actor_not_hit`。
  - `allow_no_hit=true`：返回成功，`hit=false`，且不修改任何编辑器状态。

### `viewport_select_actor_at_screen`

- `screen_x` / `screen_y`：屏幕像素坐标。
- `selection_mode`：可选，`replace / add / remove`，默认 `replace`。
- 其它 Trace 参数与 `viewport_pick_actor_at_screen` 相同。

行为说明：

- 会先从屏幕点拾取 Actor。
- 成功后按 `selection_mode` 更新编辑器当前选择集。
- 返回命中 Actor 摘要、完整 `actor_info`、命中信息，以及更新后的选择集。
- 若 `allow_no_hit=true` 且未命中，则不会修改选择集，只返回当前选择集（同时 `hit=false`）。

### `level_get_nearby_actor_obbs`

- `radius`：球形查询范围半径。
- 查询中心二选一：
  - `id`：中心 Actor 名称或标签（以该 Actor 的 bounds center 作为球心；若 bounds 无效则退回 Actor location）。
  - `center`：显式指定世界坐标球心（此模式下 `include_self` 无意义，会被忽略）。
- `include_self`：可选，仅 `id` 模式有效；是否包含中心 Actor，默认 `false`。
- 可选过滤参数（减少噪声，建议白盒流程默认填）：
  - `folder_path_prefix` / `accept_folder_path_prefix`：仅返回该 folder 前缀下 Actor
  - `ignore_folder_path_prefix`：忽略该 folder 前缀下 Actor
  - `accept_tags[]` / `ignore_tags[]`
  - `accept_class_substrings[]` / `ignore_class_substrings[]`：对 `Class->GetPathName()` 做包含匹配
  - `limit`：最多返回数量（0 表示不限制）

行为说明：

- OBB 基于 Actor 局部轴（`axis_x/axis_y/axis_z`），支持 **非均匀缩放 + 旋转** 的白盒几何（例如斜坡/倾斜平台）。
- 返回范围内 Actor 列表，每项都包含：
  - `actor`
  - `distance`
  - `obb.center`
  - `obb.axis_x / axis_y / axis_z`
  - `obb.half_lengths`
  - `obb.corners`

### `navmesh_build`

- `wait_for_finish`：可选，是否等待构建结束，默认 `false`。
- `timeout_seconds`：可选，仅在 `wait_for_finish=true` 时生效，默认 `10.0`。

行为说明：

- 在 Editor World 触发 `UNavigationSystemV1::Build()`。
- 需要关卡内存在有效 `NavMeshBoundsVolume` 才会生成可查询 Nav 数据；否则可能返回成功但 `navmesh_find_path` 仍无法投影/查路。
- `wait_for_finish=true` 会最多等待 `timeout_seconds`，并返回 `build_in_progress` 供调用方决定是否重试。

### `navmesh_project_point`

- `point`：必填，世界坐标点（也兼容字段名 `location`）。
- `project_query_extent`：可选，投影查询盒体半尺寸（cm），默认 `50,50,200`；也兼容字段名 `project_query_extent_cm`。

行为说明：

- 在 Editor World 调用 `UNavigationSystemV1::ProjectPointToNavigation`。
- 返回 `projected=true/false`：
  - `true`：附带 `projected_point`。
  - `false`：只表示“在给定 extent 下无法投影到 NavMesh”，不是硬错误（便于脚本在查路前做诊断）。
- 典型用途：当你怀疑 `navmesh_find_path(project_to_nav=true)` 把点投影到楼梯台阶/上层平台时，用此命令先把“投影落点”结构化输出，避免误判。

### `navmesh_find_path`

- `start` / `end`：必填，世界坐标。
- `allow_partial`：可选，是否接受 partial path，默认 `false`。
- `project_to_nav`：可选，是否把输入点投影到 Nav 上，默认 `true`。
- `project_query_extent`：可选，投影查询盒体半尺寸（cm），默认 `50,50,200`。
- `allow_projection_failure`：可选，默认 `false`；为 `true` 时，投影失败不作为硬错误，而是返回 `path_found=false` 并附带投影信息（用于诊断“点不在 Nav 上/投影不到”）。

行为说明：

- 返回 `path_found`、`is_partial`、`path_length_cm`、`path_points[]`，并返回：
  - `start_used/end_used`：实际用于查路的点（可能是投影点）
  - `start_projected_ok/end_projected_ok`、`*_projected_distance_cm`
  - `projection_failed_reason`（仅当 `allow_projection_failure=true` 且投影失败时）
- 注意：NavMesh/查路只覆盖“沿可行走面连续移动”的连通（walkable connectivity）。跳跃/电梯/滑索等连通需要额外建模为显式边（建议见 `Plugins/UeAgentInterface/docs/Proposed_WhiteboxValidation_Commands.md`）。

### `navmesh_spawn_bounds_volume`

- `bounds`：必填对象。
  - `bounds.center`：必填，世界坐标中心点。
  - `bounds.extent`：必填，世界坐标半尺寸（cm），三轴必须为正。
- `rotation`：可选，默认 `0,0,0`。
- `label`：可选，Actor Label。
- `folder_path`：可选，Outliner folder。
- `update_navigation_bounds`：可选，是否调用 `UNavigationSystemV1::OnNavigationBoundsUpdated`，默认 `true`。

行为说明：

- 生成 `ANavMeshBoundsVolume` 并创建对应 Brush（否则 bounds 可能为 0，NavMesh 构建/投影/查路都会失败）。
- 返回 `created_actor` 与最终 `bounds`（便于脚本侧复核“体积是否真的生效”）。
- 推荐做法：把 NavBounds 放在独立 folder（例如 `.../Nav`），截图/构图时用 `viewport_frame_folder` 聚焦 `.../Geo`，避免 NavBounds 把整体 bounds 拉大导致缩得太远。

### `level_validate_connectivity`

对白盒常用的“按序/按对”连通性验收（基于 NavMesh），并支持把“跳跃/传送/梯子/设备”等 **非 walk 连接** 作为显式边纳入同一张图里做验收：

- 输入二选一：
  - `points[]`：世界坐标点列表（建议使用 **脚底点/落脚点**）
  - `probe_actor_ids[]`：探针 Actor（如 `TargetPoint`）的名称或 Label
- `pairs[]`：可选，显式指定需要验收的“边”；每项 `{from_index,to_index}`，并可选 `edge_type`：
  - `edge_type` 省略/空/`walk`：走 NavMesh 查路（旧行为）
  - `edge_type!=walk`：显式边 **只做端点校验**（投影/距离阈值），不查路、不验证设备语义；用于把 jump/teleport/ladder 等连接纳入“整体可达图”的验收
  - 不提供 `pairs[]` 时默认按序验收 `0->1,1->2,...`
- `project_to_nav`：可选，默认 `true`；会先把点投影到 Nav 上再查路。
- `project_query_extent`：可选，默认 `50,50,200`。
- `max_projection_distance_cm`：可选，默认 `-1`（不限制）。若设置为 `>=0`，当某点投影到 Nav 的距离超过该阈值时，会视为失败（用于避免“投影到了别的楼层/别的走道”导致的误判）。
- `allow_partial`：可选，默认 `false`。
- `stop_on_failure`：可选，默认 `false`；为 `true` 时遇到第一对失败就提前返回（更快）。
- `include_path_points`：可选，默认 `false`；为 `true` 时在 `walk_checked=true` 的 pair 内返回 `path_points`（最多 `max_path_points`，并返回 `path_points_truncated`）。
- `graph_root_index`：可选，默认 `0`；基于通过的边构建有向图并返回 `graph`（可达性 + SCC，用于验收“回环/返回路径”）。

返回：

- `all_connected`：是否全部 pairs 都通过（walk 边=查路成功；显式边=端点校验通过）
- `pairs_truncated/first_failure_pair_index/first_failure_reason`：当 `stop_on_failure=true` 时用于定位第一处失败
- `nodes[]`：节点来源（点或 actor 摘要）
- `pairs[]`：逐对结果（包含投影落点、`*_projected_distance_cm`、`*_projection_within_limit`、`edge_type`、`walk_checked`、path_found、failure_reason、path_length_cm 等）
- `graph`：图验收摘要（`reachable_indices/unreachable_indices/sccs/has_cycle`）

### `level_trace_world_ray`

- `start`：必填，射线起点。
- `direction`：必填，射线方向（会自动归一化）。
- `trace_distance`：可选，最大距离；也兼容 `distance` 字段。
- `trace_channel`：可选，默认 `Visibility`。
- `trace_complex`：可选，默认 `true`。
- `ignore_actor_ids`：可选，忽略的 Actor 名称/标签数组。
- `ignore_folder_path_prefix` / `ignore_tags[]` / `ignore_class_substrings[]`：可选，全局忽略过滤器（按 Outliner folder 前缀 / Tag / ClassPath 子串忽略）；主要用于白盒增量检查时“屏蔽噪声对象”（例如天空盒、装饰组、临时调试物）。
- `include_all_hits`：可选，默认 `false`；为 `true` 时会用 `LineTraceMulti` 并返回 `hits[]`（按 `distance` 排序，用于诊断“被谁遮挡/穿过了哪些对象”）。注意：`LineTraceMulti` 的返回通常只覆盖**起始重叠 + 到第一次阻挡命中为止**的 hit（不会穿过阻挡继续返回后面的碰撞）。
- `max_hits`：可选，默认 `32`；`include_all_hits=true` 时最多返回多少条命中。
- `include_actor_folder_tags`：可选，默认 `false`；为 `true` 时会在主命中补充 `hit_actor_folder_path/hit_actor_tags`，并在 `hits[]` 内补充 `actor.folder_path/tags`，便于快速定位挡路物。

批量模式（可选）：

- `rays[]`：对象数组；每项至少包含 `start/direction`，可选覆盖 `trace_distance/trace_channel/trace_complex/ignore_actor_ids`。
- 同样支持在单项里覆盖：`include_all_hits/max_hits/include_actor_folder_tags`。
- `max_items`：可选，最多处理条数；超出会截断并返回 `truncated=true`（默认 `4096`）。
- `continue_on_error`：可选，默认 `false`；为 `true` 时会把单项参数错误记录为 `ok=false + error`，不中断整批。

返回批量模式时：`mode=batch`，并返回 `rays[]`（每项含 `index/ok/error/hit/...`）以及 `hit_count/error_count/first_error_index` 等汇总字段。

行为说明：

- 不依赖 viewport，从任意世界点发起 LineTrace。
- 返回 `hit`、命中 `location/normal/distance`，以及 `actor_name/actor_id/component_*` 等。
- 当 `include_all_hits=true` 时，额外返回：
  - `hit_count`、`hits_truncated`、`hits[]`（每项包含 `blocking_hit/time/distance/location/normal` 与 actor/component 摘要）
  - `include_actor_folder_tags=true` 时还会补充 `hit_actor_folder_path/hit_actor_tags` 与 `hits[].actor.folder_path/tags`

### `level_snap_to_surface`

把某个 Actor 沿射线吸附到命中面（常用于修复：悬空、贴合差、门缝、电梯平台不贴合地面等）。

- `id`：必填，目标 Actor 名称或标签。
- 可选：
  - `start`：射线起点（默认用 Actor 当前 location）
  - `direction`：射线方向（默认 `0,0,-1`）
  - `trace_distance`：默认 `100000`
  - `trace_channel`：默认 `Visibility`
  - `trace_complex`：默认 `true`
  - `ignore_actor_ids[]`：忽略的 Actor 名称/标签数组
  - `offset_cm`：命中点偏移（cm）
  - `offset_mode`：`normal/direction`（默认 `normal`）
  - `align_rotation`：是否按命中法线对齐 Actor 旋转（默认 `false`，白盒通常只吸附位置即可）

返回：`snapped=true/false`，以及命中信息、`previous_location/rotation`、`new_location/rotation`。

### `level_sweep_capsule`

- `start` / `end`：必填，Sweep 起点/终点。
- `radius_cm` / `half_height_cm`：必填，胶囊尺寸（cm）。
- `trace_channel`：可选，默认 `Pawn`。
- `trace_complex`：可选，默认 `false`。
- `find_initial_overlaps`：可选，默认 `true`。
- `ignore_actor_ids`：可选，忽略的 Actor 名称/标签数组。
- `ignore_folder_path_prefix` / `ignore_tags[]` / `ignore_class_substrings[]`：可选，全局忽略过滤器（按 Outliner folder 前缀 / Tag / ClassPath 子串忽略），用于减少噪声与误报。
- `include_all_hits`：可选，默认 `false`；为 `true` 时会用 `SweepMulti` 并返回 `hits[]`（按 `time` 排序，便于诊断“到底撞到了哪些东西”）。注意：`SweepMulti` 的返回通常只覆盖**起始重叠 + 到第一次阻挡命中为止**的 hit（不会“穿过阻挡”继续返回后续碰撞）。
- `max_hits`：可选，默认 `32`；`include_all_hits=true` 时最多返回多少条命中（防止 JSON 过大）。
- `include_actor_folder_tags`：可选，默认 `false`；为 `true` 时会在主命中与 `hits[]` 里补充 `hit_actor_folder_path/hit_actor_tags`（以及 `hits[].actor.folder_path/tags`），用于快速定位“是谁挡住了路”。
- `return_penetration_depth`：可选，默认 `false`；为 `true` 且 `start_penetrating=true` 时返回 `penetration_depth_cm`（用于判断“卡进去多深”）。

批量模式（可选）：

- `sweeps[]`：对象数组；每项至少包含 `start/end`，可选覆盖 `radius_cm/half_height_cm/trace_channel/trace_complex/find_initial_overlaps/ignore_actor_ids`。
  - 若单项未填写 `radius_cm/half_height_cm`，会回退使用顶层同名字段作为默认值。
- 同样支持在单项里覆盖：`include_all_hits/max_hits/include_actor_folder_tags/return_penetration_depth`。
- `stop_on_blocking_hit`：可选，默认 `false`；遇到第一段 `blocking_hit=true` 则提前停止并返回 `truncated=true`。
- `max_items`：可选，最多处理条数；超出会截断并返回 `truncated=true`（默认 `4096`）。
- `continue_on_error`：可选，默认 `false`；为 `true` 时会把单项参数错误记录为 `ok=false + error`，不中断整批。

返回批量模式时：`mode=batch`，并返回 `sweeps[]`（每项含 `index/ok/error/blocking_hit/...`）以及 `blocking_hit_count/first_blocking_index/error_count` 等汇总字段。

行为说明：

- 用胶囊做 Sweep 通行性检测（门洞太低、走廊太窄、电梯轨迹被挡等）。
- 返回 `blocking_hit`、`start_penetrating`，以及命中信息字段（同 Trace）。
- 实战建议：做“净空/通行”验证时，`start/end.z` 建议在地面上方留一点余量（例如 `+5cm`），或设 `find_initial_overlaps=false`，避免“刚好贴地”触发 `start_penetrating` 的误判。
- 当 `start_penetrating=true` 时，会额外返回 `start_penetrating_advice`（包含结构化建议：抬高 Z / 关闭 `find_initial_overlaps` 等）。
- 若需要更强的可解释性（例如生成“阻挡原因报告”），可开 `include_all_hits=true` 并结合 `include_actor_folder_tags=true`。

### `level_sweep_capsule_path`

- `points`：必填，至少 2 个点；每点为 `{"x","y","z"}`。默认语义为 **脚底点（落脚点）**。
- `points_mode`：可选，`feet/center`，默认 `feet`。
  - `feet`：`points[]` 视为脚底点；若 `snap_to_floor=true` 会先向下 Trace 修正到落脚面，再转换为胶囊中心点路径做 Sweep。
  - `center`：`points[]` 直接视为胶囊中心点路径（高级用法）。
- `radius_cm` / `half_height_cm`：必填，胶囊尺寸（cm）。
- `step_cm`：可选，采样步长（cm），默认 `50`。
- `max_samples`：可选，最大采样点数量，默认 `2048`（超出会截断并返回 `truncated=true`）。
- `trace_channel`：可选，默认 `Pawn`。
- `trace_complex`：可选，默认 `false`。
- `find_initial_overlaps`：可选，默认 `true`。
- `return_penetration_depth`：可选，默认 `false`；为 `true` 且 `start_penetrating=true` 时返回 `first_penetration_depth_cm`（以及 `segments[].penetration_depth_cm`）。
- `include_actor_folder_tags`：可选，默认 `false`；为 `true` 时在首个阻挡命中返回 `hit_actor_folder_path/hit_actor_tags`，并在 `segments[]` 的阻挡段同样补充，便于定位“哪个对象挡住了”。
- `ignore_actor_ids`：可选，忽略的 Actor 名称/标签数组。
- `ignore_folder_path_prefix` / `ignore_tags[]` / `ignore_class_substrings[]`：可选，全局忽略过滤器（按 Outliner folder 前缀 / Tag / ClassPath 子串忽略）。

贴地与擦地误报相关参数（主要对 `points_mode=feet` 有意义）：

- `snap_to_floor`：可选，默认 `true`（仅 `feet` 模式默认开启）；把脚底点向下 Trace 到真实落脚面。
- `require_floor`：可选，默认 `true`（当 `feet + snap_to_floor`）；若某个采样点找不到落脚面，会返回 `path_valid=false`、`floor_missing=true` 并早停。
- `floor_trace_up_cm` / `floor_trace_down_cm`：可选，默认 `50/200`；控制落脚面 Trace 的上下范围（建议保持较小，避免吸到别的楼层）。
- `floor_trace_channel`：可选，默认 `Visibility`。
- `floor_trace_complex`：可选，默认 `true`。
- `floor_clearance_cm`：可选，默认 `2`；把胶囊中心点抬离地面一点，减少 `start_penetrating` 误报。
- `ignore_walkable_floor_hits`：可选，默认 `false`；忽略“像可行走地面”的命中（需配合 `snap_to_floor` 才可靠）。
- `max_walkable_slope_deg`：可选，默认 `45`；用于判断命中法线是否可行走。

调试输出控制：

- `stop_on_blocking_hit`：可选，默认 `true`；遇到第一个阻挡就停止（更快）。
- `include_samples`：可选，返回采样点（input/feet/center + floor hit 信息）。
- `include_segments`：可选，返回每段 sweep 的命中与过滤统计（调试用，数据量更大）。

行为说明：

- 适合做“走廊/楼梯/电梯路径净空”的自动验收：把“中途被挡”变成可定位的 `segment_index` + 命中信息。
- 由于 `points_mode=feet` 更易生成（可从 OBB 顶面点派生），但必须明确“脚底→胶囊中心”的转换策略，否则会出现大量擦地误报；因此建议默认开启 `snap_to_floor + floor_clearance_cm`。
- 当阻挡段 `start_penetrating=true` 时，会在对应 `segments[]`（以及顶层首个阻挡信息）返回 `start_penetrating_advice`，提示如何降低擦地/初始重叠的误报。

### `level_check_overlaps`

- `shape`：必填，`box/sphere/capsule`。
- `center`：必填，世界坐标中心点。
- `box_extent`：当 `shape=box` 必填。
- `radius_cm`：当 `shape=sphere/capsule` 必填。
- `half_height_cm`：当 `shape=capsule` 必填。
- `rotation`：可选（box/capsule），默认 `0,0,0`。
- `trace_channel`：可选，默认 `Visibility`。
- `trace_complex`：可选，默认 `false`。
- `limit`：可选，最多返回 overlap 条数，默认 `100`。
- `include_overlaps`：可选，默认 `true`；为 `false` 或 `limit=0` 时只返回 `overlap_count`（不回传 `overlaps[]`，避免 JSON 太大）。
- `include_actor_folder_tags`：可选，默认 `false`；为 `true` 时会在 `overlaps[].actor` 补充 `folder_path/tags`。
- `ignore_actor_ids`：可选，忽略的 Actor 名称/标签数组。
- `ignore_folder_path_prefix` / `ignore_tags[]` / `ignore_class_substrings[]`：可选，全局忽略过滤器（按 Outliner folder 前缀 / Tag / ClassPath 子串忽略）。

批量模式（可选）：

- `checks[]`：对象数组；每项至少包含 `shape/center`，并按 shape 补齐所需字段；可选覆盖 `trace_channel/trace_complex/limit/ignore_actor_ids`。
- 同样支持在单项里覆盖：`include_overlaps/include_actor_folder_tags`。
- `stop_on_overlap`：可选，默认 `false`；遇到第一项 `overlap_count>0` 则提前停止并返回 `truncated=true`。
- `max_items`：可选，最多处理条数；超出会截断并返回 `truncated=true`（默认 `4096`）。
- `continue_on_error`：可选，默认 `false`；为 `true` 时会把单项参数错误记录为 `ok=false + error`，不中断整批。

返回批量模式时：`mode=batch`，并返回 `checks[]`（每项含 `index/ok/error/overlap_count/...`）以及 `total_overlap_count/first_overlapping_index/error_count` 等汇总字段。

行为说明：

- 返回 `overlap_count` 与 `overlaps[]`（每项包含 actor 摘要与 component 信息）。
- 用于发现摆放穿插、互相卡住、门洞/墙体不贴合等问题。

### `editor_list_dirty_resources`

- 无参数。

行为说明：

- 返回当前编辑器里所有待处理的脏包资源。
- 资源类型统一抽象为 `resource`，可能是：
  - 当前关卡
  - 其它关卡
  - 普通资产
- 返回项包含 `resource_path`、`object_path`、`kind`、`is_current_level`、`is_open_in_editor` 等字段。

### `asset_import_fbx_skeletal_mesh`

- `source_filename`：必填，外部 FBX 文件绝对路径。
- `destination_path`：必填，目标内容路径（如 `/Game/Characters/Paladin/Mesh`）。
- `skeleton_path`：可选，已有 Skeleton；不填时按 FBX 内容导入新 Skeleton。
- `replace_existing` / `replace_existing_settings`：可选，默认 `false`。
- `save_after_import`：可选，默认 `true`。
- `open_editor`：可选，默认 `false`。
- `import_materials` / `import_textures`：可选，默认 `false`。
- `create_physics_asset`：可选，默认 `true`。
- `import_animations`：可选，默认 `false`。

行为说明：

- 通过 FBX 工厂自动导入 Skeletal Mesh。
- 返回 `imported_object_paths`，并按类型拆出 `imported_skeletal_mesh_paths / imported_skeleton_paths / imported_physics_asset_paths`。
- 适合先导入角色网格，再把动作批量导入到同一 Skeleton。

### `asset_import_fbx_animation`

- `source_filename`：必填，外部 FBX 文件绝对路径。
- `destination_path`：必填，目标内容路径（如 `/Game/Characters/Paladin/Animations`）。
- `skeleton_path` / `skeletal_mesh_path`：二选一必填。
  - 若只给 `skeletal_mesh_path`，命令会自动解析其绑定 Skeleton。
- `replace_existing` / `replace_existing_settings`：可选，默认 `false`。
- `save_after_import`：可选，默认 `true`。
- `open_editor`：可选，默认 `false`。

行为说明：

- 导入结果是 `AnimSequence` 资产。
- 返回 `skeleton_path` 与 `imported_animation_paths`，便于后续直接接 `AnimBlueprint` / Montage / 结构化动画工作流。

### `asset_export_property_json`

- 这是当前“小型浅层资产”的主工作流；能被 `properties[]` 表达的逐条纯属性 setter 已迁移到对应 `deprecatedCommand` 分册。
- 对字段较多、但仍适合单文件 JSON 的资产，推荐先最小创建，再导出属性 JSON，当作真实模板继续补全。

- `asset_path`：必填，目标资产路径。
- `property_names[]`：可选；不填时会按资产类型使用内建默认预设。
- `output_file`：可选；若提供，会把完整 JSON 直接写到该路径。

当前默认预设：

- `AnimSequence`
  - `RateScale`
  - `bEnableRootMotion`
  - `bForceRootLock`
  - `RootMotionRootLock`
  - `Interpolation`
- `Texture`
  - `SRGB`
  - `CompressionSettings`
  - `MipGenSettings`
  - `LODGroup`
  - `NeverStream`
  - `VirtualTextureStreaming`
  - `Filter`
- `StaticMesh`
  - `LightMapResolution`
  - `LightMapCoordinateIndex`
  - `LODGroup`
  - `bAllowCPUAccess`
- `SkeletalMesh`
  - `bEnablePerPolyCollision`
  - `LODSettings`
  - `DefaultAnimatingRig`

行为说明：

- 返回的 JSON 结构固定包含：
  - `format_version`
  - `asset_path`
  - `object_path`
  - `asset_class`
  - `property_preset`
  - `properties[]`
- `properties[]` 每项包含：
  - `property_name`
  - `value_text`
  - `cpp_type`
- 如果某个默认属性在当前资产上不存在，会落到 `missing_properties[]`，不会中断整条导出。

### `asset_apply_property_json`

- 这是与 `asset_export_property_json` 成对的主回写入口。
- 推荐流程是：导出 JSON -> 修改 `value_text` -> 回写，而不是长链路反复调用原子属性命令。
- 推荐方法论：
  - 先创建或导入最小可用资产
  - 再 `export_property_json`
  - 再在导出结果上补高价值字段
  - 再 `apply_property_json`

- `asset_path`：可选；若不填，可从 `json_file` 内的 `asset_path` 读取。
- `json_file`：可选；指向 `asset_export_property_json` 导出的 JSON 文件。
- `properties[]`：可选；不走文件时可直接内联属性数组。
- `save_after_apply`：可选，默认 `false`。

行为说明：

- `json_file` 和 `properties[]` 至少要提供一种。
- `properties[]` 每项格式：
  - `property_name`
  - `value_text`
- 返回包含 `property_results[]`。每个结果会记录 `requested_value_text`、`applied_value_text`、`property_import_status`、`property_import_verified`、`value_text_exact_match`、`value_text_changed_after_import`、`cpp_type`。
- 如果某项 `ImportText` 解析失败，命令失败，`property_results[]` 会保留已处理项和失败项，失败项的 `property_import_status=import_failed`，错误字符串会包含 `property_name` 和请求值。
- 如果 `properties[]` 中某项不是 JSON object，命令失败，`property_results[]` 会包含 `property_import_status=invalid_property_entry`，避免坏条目被静默跳过。
- 当前会复用对象子属性路径解析，因此像 `CharacterMovement.*` 这类对象链属性也能沿同一套机制继续扩展到资产侧。
- 这条命令适合做“导出 JSON -> 手工或脚本改 `value_text` -> 回写资产”的轻量结构化工作流，不等于完整 folder profile。

### 通用属性写入返回

以下原子属性命令在成功和解析失败时都会返回属性写入观测字段：

- `actor_set_property` / `level_set_actor_property`
- `component_set_property` / `level_set_component_property`

字段包含 `requested_value_text`、`applied_value_text`、`property_import_status`、`property_import_verified`、`value_text_exact_match`、`value_text_changed_after_import`、`cpp_type`。`value_text_changed_after_import=true` 表示 UE 读回值与请求字符串不完全一致，需要结合属性类型判断是正常规范化还是值被回退。

### `editor_resolve_dirty_resources`

- `save_resource_paths` / `discard_resource_paths`：资源路径数组。
- 兼容旧字段：`save_asset_paths` / `discard_asset_paths`。
- 兼容别名：`save_resources` / `discard_resources`
- `save_current_level` / `discard_current_level`
- `save_all_dirty` / `discard_all_dirty`
- 兼容别名：`save_all` / `discard_all`
- `close_all_asset_editors`
- `only_save_dirty`

行为说明：

- 只处理脏资源，不会直接关闭编辑器。
- 返回处理前后 `dirty_resources_before / dirty_resources_after`。
- 若还有未处理的脏资源，会通过 `remaining_dirty_resource_count` 显式告诉调用方。

### `editor_close`

- `request_exit`：是否真正请求关闭编辑器，默认 `true`。
- `close_all_asset_editors`：关闭前是否先关闭资产编辑器，默认 `true`。

行为说明：

- 该命令不会替调用方做保存/丢弃判断。
- 若仍存在未处理脏资源，会返回失败 `editor_has_unresolved_dirty_resources`，并在 `data.dirty_resources` 中给出完整清单。
- 只有当脏资源全部处理完后，才会允许关闭编辑器。

## Landscape

| 指令 | 作用 | 关键参数 | 典型用途 |
|---|---|---|---|
| `landscape_create` | 在当前关卡创建 Landscape | `location`、`scale`、`quads_per_section`、`sections_per_component`、`component_count_x`、`component_count_y` | 初始化测试地形 |
| `landscape_raise_circle` | 抬高圆形区域 | `center`、`radius_cm`、`strength_cm`、`falloff` | 快速做起伏地形 |

## 最小请求示例

```json
{
  "request_id": "level-001",
  "command": "level_get_actor_transform",
  "params": {
    "id": "MyActor"
  }
}
```
