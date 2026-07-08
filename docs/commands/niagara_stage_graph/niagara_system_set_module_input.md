---
command: niagara_system_set_module_input
category: niagara_stage_graph
title: **原子/兼容**：设置 System stage module input；新 authoring 优先用 folder JSON
summary: **原子/兼容**：设置 System stage module input；新 authoring 优先用 folder JSON
aliases: []
tags: [niagara_stage_graph]
status: active
---

# `niagara_system_set_module_input`

## LLM Compact

- status: active.
- purpose: **原子/兼容**：设置 System stage module input；新 authoring 优先用 folder JSON
- key_inputs: `asset_path`, `input_name`, `module_node_guid`, `value_text`
- key_returns: `applied_value_text`, `property_import_status`, `requested_value_text`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。
- `input_name`：名称字段；同名对象存在时应结合路径或 id 消歧。
- `module_node_guid`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `value_text`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- `applied_value_text`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `property_import_status`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `requested_value_text`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
