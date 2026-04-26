# 指令详解：StaticMesh / EnhancedInput

> 废弃写入命令已迁移到 `deprecatedCommand/04_StaticMesh_EnhancedInput.md`；本分册只保留主流程、读取、导出/应用、编译、诊断，以及尚未被 JSON / 结构化 JSON 覆盖的命令。

## StaticMesh

| 指令 | 作用 | 关键参数 | 典型用法 |
|---|---|---|---|
| `static_mesh_open_editor` | 打开静态网格编辑器 | `asset_path` | 进入网格编辑上下文 |
| `static_mesh_get_info` | 读取网格信息（含 bounds / socket / 碰撞 / 预览） | `asset_path` | 修改前读取整体状态 |
| `static_mesh_get_bounds` | 只读取静态网格 bounds | `asset_path` | 供脚本直接拿宽高深与中心 |
| `static_mesh_get_local_corners` | 读取局部包围盒 8 个角点 | `asset_path` | 做拼装、特征点对齐、预估占地 |
| `static_mesh_set_preview_view` | 设置网格编辑器预览相机 | `asset_path`、`yaw`、`pitch`、`distance`、`fov` | 多角度检查模型 |
| `static_mesh_set_material_slot` | 设置默认材质槽材质 | `asset_path`、`slot_index` 或 `slot_name`、`material_path` | 调整网格默认材质 |
| `static_mesh_set_collision_boxes` | 设置简单碰撞 Box 列表 | `asset_path`、`boxes[]`、`clear_other_shapes` | 重建简单碰撞体 |
| `static_mesh_set_collision_spheres` | 设置简单碰撞 Sphere 列表 | `asset_path`、`spheres[]`、`clear_other_shapes` | 补球形触发体或球形包络 |
| `static_mesh_set_collision_capsules` | 设置简单碰撞 Capsule 列表 | `asset_path`、`capsules[]`、`clear_other_shapes` | 补柱体/角色近似碰撞 |
| `static_mesh_add_socket` | 新增 Socket | `asset_path`、`socket_name`、`location`、`rotation`、`scale` | 武器挂点、特效挂点 |
| `static_mesh_update_socket` | 修改 Socket | `asset_path`、`socket_name`、`new_socket_name` | 调整挂点位置 |
| `static_mesh_remove_socket` | 删除 Socket | `asset_path`、`socket_name` | 清理无效挂点 |

`static_mesh_set_property` 返回 `requested_value_text`、`applied_value_text`、`property_import_status`、`property_import_verified`、`value_text_exact_match`、`value_text_changed_after_import`、`cpp_type`。如果 `ImportText` 失败，返回数据会保留失败的 `property_name` 和请求值。

### `static_mesh_get_bounds` 与 `static_mesh_get_local_corners`

- `static_mesh_get_info` 原本已经包含 `bounds`
- 新增这两个命令的目的，是让脚本在批量搭建和对齐时少解析无关字段

建议：

- 只想知道模型尺寸、中心、包围盒：用 `static_mesh_get_bounds`
- 想做角点级几何判断：用 `static_mesh_get_local_corners`
- 想看完整网格摘要：用 `static_mesh_get_info`

### `static_mesh_get_info`

当前还会返回：

- `material_slot_count`
- `material_slots[]`
  - `slot_index`
  - `slot_name`
  - `imported_slot_name`
  - `material_path`
  - `overlay_material_path`
- `collision.spheres[]`
- `collision.capsules[]`

### `static_mesh_set_material_slot`

- `slot_index` 和 `slot_name` 二选一。
- `slot_name` 会同时匹配 `MaterialSlotName` 和 `ImportedMaterialSlotName`。
- 当前只处理默认材质槽，不处理实例级 `StaticMeshComponent` 覆盖材质；实例覆盖仍应走 `level_set_component_property`。

### 简单碰撞补充

- `static_mesh_set_collision_boxes / spheres / capsules` 都是“重设该形状数组”的语义，不是追加。
- `clear_other_shapes=true` 时，会一并清掉其它简单碰撞形状和现有 convex。
- `static_mesh_set_collision_capsules` 的 `length` 是胶囊中间圆柱段长度，不含两端半球。

## EnhancedInput

| 指令 | 作用 | 关键参数 | 典型用法 |
|---|---|---|---|
| `enhanced_input_create_action` | 创建 `InputAction` | `asset_path`、`value_type` | 新建 Move / Look / Jump 动作 |
| `enhanced_input_get_action_info` | 读取 `InputAction` 信息 | `asset_path` | 校验值类型与触发配置 |
| `enhanced_input_export_action_json` | 导出 `InputAction` 单文件 JSON | `asset_path`；可选 `output_file` | 把小型 Action 资产拉成单个真源 JSON |
| `enhanced_input_apply_action_json` | 从单文件 JSON 回写 `InputAction` | `json_file`；可选 `asset_path`、`create_if_missing` | 批量改值类型、布尔标志与 action 级触发器/修饰器 |
| `enhanced_input_create_mapping_context` | 创建 `InputMappingContext` | `asset_path` | 新建一套输入映射 |
| `enhanced_input_get_mapping_context_info` | 读取 `MappingContext` 信息 | `asset_path` | 列出现有映射关系 |
| `enhanced_input_export_mapping_context_json` | 导出 `InputMappingContext` 单文件 JSON | `asset_path`；可选 `output_file` | 把整套映射关系导出成单个真源 JSON |
| `enhanced_input_apply_mapping_context_json` | 从单文件 JSON 回写 `InputMappingContext` | `json_file`；可选 `asset_path`、`create_if_missing` | 以 JSON 真源重建 mappings[] |

`enhanced_input_set_action_property` / `enhanced_input_set_mapping_context_property` 返回通用属性写入观测字段：`requested_value_text`、`applied_value_text`、`property_import_status`、`property_import_verified`、`value_text_exact_match`、`value_text_changed_after_import`、`cpp_type`。

### EnhancedInput JSON 工作流

- 这是 `InputAction / InputMappingContext` 的主编辑工作流。
- 当前不再推荐把这两类小资产当成“逐条 add/remove/set 命令”来长期 authoring。
- `InputAction` 和 `InputMappingContext` 当前不走文件夹式真源。
- 这两类资产内容小、层级浅，推荐直接使用“单资产 = 单 JSON”的工作流。
- `enhanced_input_export_*_json` 会导出 UTF-8 单文件 JSON。
- `enhanced_input_apply_*_json` 会从 JSON 回写；当 `modifier_classes / trigger_classes / mappings` 字段存在时，语义是“按 JSON 全量替换该数组”。
- `json_file` 文件缺失、读取失败或 JSON 语法解析失败会直接失败返回 `json_file_not_found / load_json_file_failed / json_parse_failed`；映射项缺 `action_path/key`、无效 key、action 不存在等 schema 问题会以明确错误码返回，不会静默跳过。

推荐方法：

1. 先最小创建 `InputAction` / `InputMappingContext`
2. 先 export 单 JSON
3. 以导出结果为模板补全字段
4. 再 apply

#### `enhanced_input_export_action_json`

- 必填：`asset_path`
- 可选：`output_file`
- 导出字段：
  - `asset_path`
  - `value_type`
  - `consume_input`
  - `trigger_when_paused`
  - `reserve_all_mappings`
  - `modifier_classes[]`
  - `trigger_classes[]`

#### `enhanced_input_apply_action_json`

- 推荐参数：`json_file`、`save_after_apply`
- 可选：`asset_path`
  - 不填时默认取 JSON 里的 `asset_path`
- 可选：`create_if_missing`
  - 默认 `true`

#### `enhanced_input_export_mapping_context_json`

- 必填：`asset_path`
- 可选：`output_file`
- 导出字段：
  - `asset_path`
  - `mappings[]`
    - `key`
    - `action_path`
    - `modifier_classes[]`
    - `trigger_classes[]`

#### `enhanced_input_apply_mapping_context_json`

- 推荐参数：`json_file`、`save_after_apply`
- 可选：`asset_path`
  - 不填时默认取 JSON 里的 `asset_path`
- 可选：`create_if_missing`
  - 默认 `true`
- 当 JSON 中存在 `mappings[]` 时，会先清空现有映射，再按 JSON 重建。

## 最小请求示例

```json
{
  "request_id": "sm-001",
  "command": "static_mesh_get_local_corners",
  "params": {
    "asset_path": "/Engine/BasicShapes/Cube.Cube"
  }
}
```
