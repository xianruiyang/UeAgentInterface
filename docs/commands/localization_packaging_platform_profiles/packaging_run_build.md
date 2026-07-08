---
command: packaging_run_build
category: localization_packaging_platform_profiles
title: Packaging
summary: 计划或执行 UAT 打包任务
aliases: []
tags: [localization_packaging_platform_profiles]
status: active
---

# `packaging_run_build`

## LLM Compact

- status: active.
- purpose: 计划或执行 UAT 打包任务
- key_inputs: `json_file`, `allow_uat`, `execute`, `json`, `timeout_seconds`
- key_returns: `log_file`, `report_file`, `status`, `task_id`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `json_file`：JSON authoring 文件路径；长 JSON 和可复用 payload 优先走文件。
- `allow_uat`：布尔/开关字段；默认值以本命令契约为准。
- `execute`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `json`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `timeout_seconds`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- `log_file`：文件路径字段；长 JSON/结果优先走文件。
- `report_file`：文件路径字段；长 JSON/结果优先走文件。
- `status`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `task_id`：稳定 id，用于精确定位或后续操作。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
