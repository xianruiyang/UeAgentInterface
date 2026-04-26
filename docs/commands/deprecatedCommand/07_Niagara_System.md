# 废弃指令：Niagara（System / Asset / User Parameter）

> 本文件从 `../07_Niagara_System.md` 抽出，保留原分册章节结构。主分册只保留 JSON / 结构化 JSON 主流程、读取、导出/应用、编译和诊断命令。

## System 属性与编译

| 指令 | 作用 | 关键参数 | 典型用法 |
|---|---|---|---|
| `niagara_set_property` | **Deprecated for authoring**：设置 System 可反射属性 | `asset_path`、`property_name`、`value_text` | 仅用于局部修补 |

## System User 参数 CRUD

| 指令 | 作用 | 关键参数 | 典型用法 |
|---|---|---|---|
| `niagara_user_parameter_add` | **Deprecated for authoring**：新增 User 参数 | `asset_path`、`parameter_name`、`parameter_type`、`default_value_text` | 仅用于局部修补 |
| `niagara_user_parameter_remove` | **Deprecated for authoring**：删除 User 参数 | `asset_path`、`parameter_name` | 仅用于局部修补 |
| `niagara_user_parameter_set` | **Deprecated for authoring**：设置 User 参数值 | `asset_path`、`parameter_name`、`value_text` | 仅用于局部修补 |

## System 内 Emitter 管理

| 指令 | 作用 | 关键参数 | 典型用法 |
|---|---|---|---|
| `niagara_system_add_emitter` | **Deprecated for authoring**：向 System 添加 Emitter | `system_asset_path`、`emitter_asset_path`、`emitter_name` | 仅用于 bootstrap/修补 |
| `niagara_system_remove_emitter` | **Deprecated for authoring**：从 System 移除 Emitter | `system_asset_path` + (`emitter_id`/`emitter_name`/`emitter_index`) | 仅用于修补 |
| `niagara_system_move_emitter` | **Deprecated for authoring**：调整 Emitter 顺序 | `system_asset_path` + emitter 标识 + `target_index` | 仅用于修补 |
| `niagara_system_set_emitter_enabled` | **Deprecated for authoring**：启用/禁用某 Emitter | `system_asset_path` + emitter 标识 + `enabled` | 仅用于诊断/修补 |
| `niagara_system_set_emitter_version` | **Deprecated for authoring**：切换 Emitter 版本 | `system_asset_path` + emitter 标识 + `emitter_version` | 仅用于修补 |
