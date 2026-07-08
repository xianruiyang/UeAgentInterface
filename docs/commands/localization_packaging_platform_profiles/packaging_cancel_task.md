---
command: packaging_cancel_task
category: localization_packaging_platform_profiles
title: Packaging
summary: 取消 planned/running 任务
aliases: []
tags: [localization_packaging_platform_profiles]
status: active
---

# `packaging_cancel_task`

## LLM Compact

- status: active.
- purpose: 取消 planned/running 任务
- key_inputs: `kill_process_tree`, `task_id`
- key_returns: `cancelled`, `processes[]`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `kill_process_tree`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `task_id`：稳定 id，用于精确定位或后续操作。

## Returns

- `cancelled`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `processes[]`：processes 列表摘要；大列表应写入 output/artifact 或 debug。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
