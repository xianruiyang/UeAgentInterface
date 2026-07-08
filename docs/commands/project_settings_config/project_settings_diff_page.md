---
command: project_settings_diff_page
category: project_settings_config
title: Project Settings
summary: 只读生成字段差异，不落盘
aliases: []
tags: [project_settings_config]
status: active
---

# `project_settings_diff_page`

## LLM Compact

- status: active.
- purpose: 只读生成字段差异，不落盘
- key_inputs: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- key_returns: `changed`, `field_diffs[]`, `field_results[]`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- 未在旧分册命令表中列出；以实现 schema/readback 为准。

## Returns

- `changed`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `field_diffs[]`：field_diffs 列表摘要；大列表应写入 output/artifact 或 debug。
- `field_results[]`：field_results 列表摘要；大列表应写入 output/artifact 或 debug。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
