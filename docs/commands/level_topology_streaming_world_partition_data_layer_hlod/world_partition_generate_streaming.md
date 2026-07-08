---
command: world_partition_generate_streaming
category: level_topology_streaming_world_partition_data_layer_hlod
title: World Partition
summary: 生成 streaming 数据，默认只返回计划
aliases: []
tags: [level_topology_streaming_world_partition_data_layer_hlod]
status: active
---

# `world_partition_generate_streaming`

## LLM Compact

- status: active.
- purpose: 生成 streaming 数据，默认只返回计划
- key_inputs: `allow_commandlet`, `execute`, `timeout_seconds`
- key_returns: `commandlet_plan`, `exit_code`, `status`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `allow_commandlet`：布尔/开关字段；默认值以本命令契约为准。
- `execute`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `timeout_seconds`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- `commandlet_plan`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `exit_code`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `status`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
