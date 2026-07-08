---
command: packaging_parse_log
category: localization_packaging_platform_profiles
title: Packaging
summary: 解析 UAT/cook log 的 error/warning
aliases: []
tags: [localization_packaging_platform_profiles]
status: active
---

# `packaging_parse_log`

## LLM Compact

- status: active.
- purpose: 解析 UAT/cook log 的 error/warning
- key_inputs: `classify_errors`, `log_file`
- key_returns: `errors[]`, `phase_summary`, `warnings[]`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `classify_errors`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `log_file`：文件路径字段；长 JSON/结果优先走文件。

## Returns

- `errors[]`：errors 列表摘要；大列表应写入 output/artifact 或 debug。
- `phase_summary`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `warnings[]`：warnings 列表摘要；大列表应写入 output/artifact 或 debug。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
