# 指令详解：Niagara Stage / Graph / Node / ModuleInput

> 废弃写入命令已迁移到 `deprecatedCommand/09_Niagara_StageGraph.md`；本分册只保留主流程、读取、导出/应用、编译、诊断，以及尚未被 JSON / 结构化 JSON 覆盖的命令。

## Stage 管理

| 指令 | 作用 | 关键参数 | 典型用法 |
|---|---|---|---|
| `niagara_emitter_list_stages` | 列出 Emitter 所有 Stage | `emitter_asset_path`、`include_modules`、`include_module_inputs` | 先拿 `stage_key` 和 `script_usage_id` |

## Stage 模块管理

| 指令 | 作用 | 关键参数 | 典型用法 |
|---|---|---|---|
| `niagara_emitter_list_stage_modules` | 列出 Stage 模块 | `emitter_asset_path` + Stage 标识 + `include_inputs` | 读取 `module_node_guid` |

## Stage 节点管理

> `niagara_emitter_set_stage_property` 和 `niagara_emitter_set_stage_node_property` 返回通用属性观测字段：`requested_value_text`、`applied_value_text`、`property_value_read_back`、`property_import_status`、`property_import_error`、`value_text_changed_after_import`、`cpp_type`。属性不存在会返回 `property_not_found`，UE `ImportText` 解析失败会返回 `property_import_failed:<property>:<value>`。

| 指令 | 作用 | 关键参数 | 典型用法 |
|---|---|---|---|
| `niagara_emitter_list_stage_nodes` | 列出 Stage 全部节点 | `emitter_asset_path` + Stage 标识 + `include_properties` | 查节点 GUID 与类型 |
| `niagara_emitter_get_stage_node_property` | 读取节点属性 | `emitter_asset_path` + Stage 标识 + `node_guid` + `property_name` | 检查节点参数 |

## 模块输入管理

| 指令 | 作用 | 关键参数 | 典型用法 |
|---|---|---|---|
| `niagara_emitter_list_module_inputs` | 列出模块输入（含隐藏输入） | `emitter_asset_path`、`module_node_guid` | 查询可写输入名 |

> 废弃写入命令已迁移到 `deprecatedCommand/09_Niagara_StageGraph.md` 的对应章节。Dynamic Input 与曲线写入已并入 `15_Niagara_FolderFormat.md` 的 folder JSON 主流程，不再通过 `niagara_emitter_set_module_input` 扩展参数制作。
## 防崩与限制（必须了解）

- `niagara_emitter_remove_stage_module`：默认软删除（禁用模块），避免直接破坏 Stack 图。
- `niagara_emitter_remove_stage_node`：模块节点走软删除，非模块节点才物理删除。
- `niagara_emitter_add_stage_node`：对 `ParticleEventScript` 的危险加模块场景有保护拦截。
- `niagara_emitter_set_module_input`：Static Switch 输入不支持参数链接，需改默认值方式。其 Dynamic Input / `curve_json` 扩展只保留兼容，不作为 authoring 或修补首选；改用 folder JSON 的 `inputs[].dynamic_input.data_interfaces[].raw_properties[].curve_json`。

## 推荐执行顺序

1. `niagara_emitter_export_folder`
2. 写基础 Stage / Module / Node / ModuleInput JSON
3. `niagara_emitter_apply_folder`
4. 再导出一次，基于 UE 补全后的完整属性面修改参数
5. 再次 apply，并检查返回的 `stack_issues` / `stack_error_count`
6. 对包含该 emitter 的 System 做 compile log 和 runtime probe

## 最小请求示例

```json
{
  "request_id": "nsg-001",
  "command": "niagara_emitter_set_module_input",
  "params": {
    "emitter_asset_path": "/Game/AutoTests/Niagara/NE_Leader",
    "module_node_guid": "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
    "input_name": "Kill",
    "link_parameter_name": "Particles.HasCollided"
  }
}
```

<!-- UAI_FORMAL_COMMAND_INDEX_BEGIN -->
## 原子/兼容命令正式索引
>
> 以下命令已有真实 dispatch 实现并纳入 smoke 覆盖；对完整资产制作仍应优先使用本分册中的结构化 JSON / folder workflow，原子命令仅用于 bootstrap、迁移、probe 或局部修复。

| Command | 作用 | 关键参数 | 关键返回 |
| --- | --- | --- | --- |
| `niagara_emitter_add_simulation_stage` | **原子/兼容**：新增 Simulation Stage | `emitter_asset_path`、`stage_class`、`stage_name`、`target_index` | 仅用于修补 |
| `niagara_emitter_add_stage_module` | **原子/兼容**：向 Stage 添加模块 | `emitter_asset_path` + Stage 标识 + `module_script_asset_path` | 仅用于修补 |
| `niagara_emitter_add_stage_node` | **原子/兼容**：添加 Stage 节点（模块/特定节点） | `emitter_asset_path` + Stage 标识 + (`module_script_asset_path` 或 `node_class`) | 仅用于修补 |
| `niagara_emitter_clear_module_input` | **原子/兼容**：清除输入覆盖，回退默认 | `emitter_asset_path`、`module_node_guid`、`input_name` | 仅用于修补 |
| `niagara_emitter_connect_stage_nodes` | **原子/兼容**：连接节点引脚 | `emitter_asset_path` + Stage 标识 + `from_node_guid/from_pin/to_node_guid/to_pin` | 仅用于修补 |
| `niagara_emitter_disconnect_stage_nodes` | **原子/兼容**：断开节点连线 | `emitter_asset_path` + Stage 标识 + `from_node_guid/from_pin` | 仅用于修补 |
| `niagara_emitter_move_stage_module` | **原子/兼容**：编辑 Niagara Stage Graph/Module Input 的节点、连线、排序或输入 | `asset_path`、`emitter_name`、`module_name`、`input_name`、`value_text` | `success`、`warnings`、`errors` |
| `niagara_emitter_remove_stage` | **原子/兼容**：删除 Stage | `emitter_asset_path` + (`stage_key` 或 `script_usage`+`script_usage_id`) | 仅用于修补 |
| `niagara_emitter_remove_stage_module` | **原子/兼容**：移除 Stage 模块（当前为软删除） | `emitter_asset_path` + (`module_node_guid` 或 `module_index/module_name`) | 仅用于修补 |
| `niagara_emitter_remove_stage_node` | **原子/兼容**：删除节点（模块节点优先软删除） | `emitter_asset_path` + Stage 标识 + `node_guid` | 仅用于修补 |
| `niagara_emitter_set_module_input` | **原子/兼容**：设置模块输入值或绑定参数 | `emitter_asset_path`、`module_node_guid`、`input_name`、`value_text`（或 `link_parameter_name`） | 仅用于局部修补 |
| `niagara_emitter_set_stage_module_enabled` | **原子/兼容**：启用/禁用模块 | `emitter_asset_path`、`module_node_guid`、`enabled` | 仅用于诊断/修补 |
| `niagara_emitter_set_stage_node_property` | **原子/兼容**：设置节点属性 | `emitter_asset_path` + Stage 标识 + `node_guid` + `property_name` + `value_text` | 仅用于局部修补 |
| `niagara_emitter_set_stage_property` | **原子/兼容**：设置 Stage 属性 | `emitter_asset_path` + Stage 标识 + `property_name` + `value_text` | 仅用于局部修补 |
| `niagara_list_module_library` | **原子/兼容**：编辑 Niagara Stage Graph/Module Input 的节点、连线、排序或输入 | `asset_path`、`emitter_name`、`module_name`、`input_name`、`value_text` | `success`、`warnings`、`errors` |
| `niagara_system_list_module_inputs` | **原子/兼容**：编辑 Niagara Stage Graph/Module Input 的节点、连线、排序或输入 | `asset_path`、`emitter_name`、`module_name`、`input_name`、`value_text` | `success`、`warnings`、`errors` |
| `niagara_system_list_stage_modules` | **原子/兼容**：编辑 Niagara Stage Graph/Module Input 的节点、连线、排序或输入 | `asset_path`、`emitter_name`、`module_name`、`input_name`、`value_text` | `success`、`warnings`、`errors` |
| `niagara_system_set_module_input` | **原子/兼容**：编辑 Niagara Stage Graph/Module Input 的节点、连线、排序或输入 | `asset_path`、`emitter_name`、`module_name`、`input_name`、`value_text` | `success`、`warnings`、`errors` |
<!-- UAI_FORMAL_COMMAND_INDEX_END -->
