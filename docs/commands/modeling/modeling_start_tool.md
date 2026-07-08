---
command: modeling_start_tool
category: modeling
title: **原子/兼容**：控制 Modeling Mode 工具、选择或网格编辑动作
summary: **原子/兼容**：控制 Modeling Mode 工具、选择或网格编辑动作
aliases: []
tags: [modeling]
status: active
---

# `modeling_start_tool`

## LLM Compact

- status: active.
- purpose: **原子/兼容**：控制 Modeling Mode 工具、选择或网格编辑动作
- key_inputs: `asset_path`, `selection`, `properties`, `target_actor`, `tool_name`
- key_returns: `errors`, `success`, `warnings`
- note: 作用：启动任意 `Modeling Tool`。
- note: 若传入 `tool_properties`，返回 `property_import_results[]`，每项包含 `requested_value_text`、`applied_value_text`、`property_value_read_back`、`property_import_status`、`property_import_verified`、`property_import_error`、`value_text_exact_match`、`value_text_changed_after_import`、`cpp_type`。
- note: 若工具存在 `CreateMeshObjectTypeProperties.OutputType` 且调用方未显式覆盖，UAI 会写入 `Dynamic Mesh`，并返回 `default_output_type_applied=true`、`default_output_type=Dynamic Mesh`。
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 作用：启动任意 `Modeling Tool`。
- 若传入 `tool_properties`，返回 `property_import_results[]`，每项包含 `requested_value_text`、`applied_value_text`、`property_value_read_back`、`property_import_status`、`property_import_verified`、`property_import_error`、`value_text_exact_match`、`value_text_changed_after_import`、`cpp_type`。
- 若工具存在 `CreateMeshObjectTypeProperties.OutputType` 且调用方未显式覆盖，UAI 会写入 `Dynamic Mesh`，并返回 `default_output_type_applied=true`、`default_output_type=Dynamic Mesh`。

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。
- `selection`：目标选择器；支持的字段由命令定义。
- `properties`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `target_actor`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `tool_name`：名称字段；同名对象存在时应结合路径或 id 消歧。

## Returns

- `errors`：错误摘要。
- `success`：命令业务是否成功；仍需按返回字段读回验收。
- `warnings`：警告摘要。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
