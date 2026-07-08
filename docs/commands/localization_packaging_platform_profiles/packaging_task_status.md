---
command: packaging_task_status
category: localization_packaging_platform_profiles
title: Packaging
summary: 查询 UAI 任务记录或当前进程状态
aliases: []
tags: [localization_packaging_platform_profiles]
status: active
---

# `packaging_task_status`

## LLM Compact

- status: active.
- purpose: 查询 UAI 任务记录或当前进程状态
- key_inputs: `task_id`
- key_returns: `exit_code`, `last_log_lines[]`, `phase`, `running`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `task_id`：稳定 id，用于精确定位或后续操作。

## Returns

- `exit_code`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `last_log_lines[]`：last_log_lines 列表摘要；大列表应写入 output/artifact 或 debug。
- `phase`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `running`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
