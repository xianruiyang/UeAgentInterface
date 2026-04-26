# 废弃指令：Niagara Stage / Graph / Node / ModuleInput

> 本文件从 `../09_Niagara_StageGraph.md` 抽出，保留原分册章节结构。主分册只保留 JSON / 结构化 JSON 主流程、读取、导出/应用、编译和诊断命令。

## Stage 管理

| 指令 | 作用 | 关键参数 | 典型用法 |
|---|---|---|---|
| `niagara_emitter_add_simulation_stage` | **Deprecated for authoring**：新增 Simulation Stage | `emitter_asset_path`、`stage_class`、`stage_name`、`target_index` | 仅用于修补 |
| `niagara_emitter_remove_stage` | **Deprecated for authoring**：删除 Stage | `emitter_asset_path` + (`stage_key` 或 `script_usage`+`script_usage_id`) | 仅用于修补 |
| `niagara_emitter_set_stage_property` | **Deprecated for authoring**：设置 Stage 属性 | `emitter_asset_path` + Stage 标识 + `property_name` + `value_text` | 仅用于局部修补 |

## Stage 模块管理

| 指令 | 作用 | 关键参数 | 典型用法 |
|---|---|---|---|
| `niagara_emitter_add_stage_module` | **Deprecated for authoring**：向 Stage 添加模块 | `emitter_asset_path` + Stage 标识 + `module_script_asset_path` | 仅用于修补 |
| `niagara_emitter_remove_stage_module` | **Deprecated for authoring**：移除 Stage 模块（当前为软删除） | `emitter_asset_path` + (`module_node_guid` 或 `module_index/module_name`) | 仅用于修补 |
| `niagara_emitter_set_stage_module_enabled` | **Deprecated for authoring**：启用/禁用模块 | `emitter_asset_path`、`module_node_guid`、`enabled` | 仅用于诊断/修补 |

## Stage 节点管理

| 指令 | 作用 | 关键参数 | 典型用法 |
|---|---|---|---|
| `niagara_emitter_add_stage_node` | **Deprecated for authoring**：添加 Stage 节点（模块/特定节点） | `emitter_asset_path` + Stage 标识 + (`module_script_asset_path` 或 `node_class`) | 仅用于修补 |
| `niagara_emitter_set_stage_node_property` | **Deprecated for authoring**：设置节点属性 | `emitter_asset_path` + Stage 标识 + `node_guid` + `property_name` + `value_text` | 仅用于局部修补 |
| `niagara_emitter_remove_stage_node` | **Deprecated for authoring**：删除节点（模块节点优先软删除） | `emitter_asset_path` + Stage 标识 + `node_guid` | 仅用于修补 |
| `niagara_emitter_connect_stage_nodes` | **Deprecated for authoring**：连接节点引脚 | `emitter_asset_path` + Stage 标识 + `from_node_guid/from_pin/to_node_guid/to_pin` | 仅用于修补 |
| `niagara_emitter_disconnect_stage_nodes` | **Deprecated for authoring**：断开节点连线 | `emitter_asset_path` + Stage 标识 + `from_node_guid/from_pin` | 仅用于修补 |

## 模块输入管理

| 指令 | 作用 | 关键参数 | 典型用法 |
|---|---|---|---|
| `niagara_emitter_set_module_input` | **Deprecated for authoring**：设置模块输入值或绑定参数 | `emitter_asset_path`、`module_node_guid`、`input_name`、`value_text`（或 `link_parameter_name`） | 仅用于局部修补 |
| `niagara_emitter_clear_module_input` | **Deprecated for authoring**：清除输入覆盖，回退默认 | `emitter_asset_path`、`module_node_guid`、`input_name` | 仅用于修补 |

### `niagara_emitter_set_module_input`

### `niagara_emitter_set_module_input`

该指令会先用 `niagara_emitter_list_module_inputs` 相同的解析逻辑定位输入：

- 如果输入是模块 FunctionCall 上的可见 pin（例如 Niagara 模块里的 `Velocity Mode`、`Cone Angle Mode` 这类枚举/静态开关输入），会直接写入该可见 pin。
- 如果输入是隐藏 stack input，会创建或复用 aliased stack override pin。
- 返回值里的 `write_target` 表示实际写入目标：`visible_pin` 或 `stack_override_pin`。
- `value_text` 会按 Niagara 输入类型规范化后写入 pin：`Vector/Position` 支持 `0 0 620`、`0,0,620`、`(X=0,Y=0,Z=620)`，实际写入为 `(X=...,Y=...,Z=...)`；`LinearColor` 支持 `0.1 0.8 1 1`、`#66CCFF`、`(R=...,G=...,B=...,A=...)`，实际写入为 `(R=...,G=...,B=...,A=...)`；`NaN/Inf` 会被拒绝。
- 返回值包含 `requested_value_text`、`applied_value_text`、`value_text_normalized`，用于区分调用方传入值和真正写入 UE pin 的结构化文本。
- 设置默认值后会做写后校验。成功返回包含 `default_value_apply_verified=true` 和 `default_value_apply_status`；如果 UE 接受了字符串但 `OverridePin.DefaultValue` 没有落成预期文本，命令会失败并返回 `pin_default_apply_verification_failed:<input>:<detail>`。这类失败通常表示 Niagara pin default 的文本解析/导入格式不匹配。
- 设置后应使用 `niagara_emitter_list_module_inputs` 回读同一 `module_node_guid`，确认 `override_default_value` 和 `has_override`。

调试建议：

- 编译日志出现 `Float constant table contains NaN or Inf` 且 `node_guid` 为空时，优先用 `niagara_emitter_set_stage_module_enabled` 临时禁用同类 Spawn 模块做二分定位。UE 5.6 中部分引擎模块资产的内部默认常量也可能触发该警告，输入回读看不到显式 `NaN`。
- 简单向上喷射、喷泉或烟雾测试不需要角锥随机速度时，可优先使用 `/Niagara/Modules/Spawn/Velocity/AddVelocity` 并设置 `Module.Velocity`，比 `AddVelocityInCone` 更容易得到无 warning 的基础回归样例。
- 如果 UI 中 `Color` 显示纯黑或 `Velocity` 显示 `0,0,0`，先回读 `applied_value_text / override_default_value`：旧格式的裸空格数值可能被 UE pin 默认值解析为黑色或零向量，应改用上述结构化文本或使用已规范化后的命令版本。
- 对 `Vector/Position/Vector4/Quat/LinearColor`，写后校验要求存储文本保持 UE import text 结构形态。即便裸文本能被工具层解析成同样数值，也不视为安全存储格式。
