---
command: modeling_set_active_tool_property
category: modeling
title: **原子/兼容**：控制 Modeling Mode 工具、选择或网格编辑动作
summary: **原子/兼容**：控制 Modeling Mode 工具、选择或网格编辑动作
aliases: []
tags: [modeling]
status: active
---

# `modeling_set_active_tool_property`

## LLM Compact

- status: active.
- purpose: **原子/兼容**：控制 Modeling Mode 工具、选择或网格编辑动作
- key_inputs: `asset_path`, `selection`, `properties`, `target_actor`, `tool_name`
- key_returns: `errors`, `success`, `warnings`
- note: 作用：设置当前活动工具属性。
- note: 返回通用属性写入观测字段：`requested_value_text`、`applied_value_text`、`property_value_read_back`、`property_import_status`、`property_import_verified`、`property_import_error`、`value_text_exact_match`、`value_text_changed_after_import`、`cpp_type`。空字符串 `value_text` 视为合法写入值，不再被当作缺参。
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 作用：设置当前活动工具属性。
- 返回通用属性写入观测字段：`requested_value_text`、`applied_value_text`、`property_value_read_back`、`property_import_status`、`property_import_verified`、`property_import_error`、`value_text_exact_match`、`value_text_changed_after_import`、`cpp_type`。空字符串 `value_text` 视为合法写入值，不再被当作缺参。

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
