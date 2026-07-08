---
command: enhanced_input_export_action_json
category: static_mesh_enhanced_input
title: EnhancedInput
summary: 导出 `InputAction` 单文件 JSON
aliases: []
tags: [static_mesh_enhanced_input]
status: active
---

# `enhanced_input_export_action_json`

## LLM Compact

- status: active.
- purpose: 导出 `InputAction` 单文件 JSON
- key_inputs: `asset_path`, `output_file`
- key_returns: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- note: 必填：`asset_path`
- note: 可选：`output_file`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 必填：`asset_path`
- 可选：`output_file`
- 返回的 `authoring_fields[]` 描述可编辑字段 metadata，不复制当前值；实际当前值仍在 action 固定字段中。
- `authoring_fields[]` 使用 `selector`、`display_name`、`category_path`、`control_type`、`value_kind`、`availability`、`operations` 和 `candidates`。

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。
- `output_file`：输出 artifact 路径；大结果应写文件，不要内联到 compact data。

## Returns

- 未在旧分册命令表中列出；以实现返回和 readback 为准。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
