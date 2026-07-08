---
command: project_settings_validate_page
category: project_settings_config
title: Project Settings
summary: 只读校验 patch/json_file 能否按当前反射 schema 写入
aliases: []
tags: [project_settings_config]
status: active
---

# `project_settings_validate_page`

## LLM Compact

- status: active.
- purpose: 只读校验 patch/json_file 能否按当前反射 schema 写入
- key_inputs: `json_file`, `fields[]`, `fields{}`, `patch{}`, `schema_mismatch_policy`, `section_id`, `values{}`
- key_returns: `valid`, `error_count`, `field_results[]`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `json_file`：JSON authoring 文件路径；长 JSON 和可复用 payload 优先走文件。
- `fields[]`：fields 列表。
- `fields{}`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `patch{}`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `schema_mismatch_policy`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `section_id`：稳定 id，用于精确定位或后续操作。
- `values{}`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- `valid`：校验是否通过。
- `error_count`：数量摘要。
- `field_results[]`：field_results 列表摘要；大列表应写入 output/artifact 或 debug。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
