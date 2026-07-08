---
command: world_partition_resave_actors
category: level_topology_streaming_world_partition_data_layer_hlod
title: World Partition
summary: 运行 resave actors commandlet，默认只返回计划
aliases: []
tags: [level_topology_streaming_world_partition_data_layer_hlod]
status: active
---

# `world_partition_resave_actors`

## LLM Compact

- status: active.
- purpose: 运行 resave actors commandlet，默认只返回计划
- key_inputs: `actor_guids[]`, `allow_commandlet`, `execute`, `timeout_seconds`
- key_returns: `commandlet_plan`, `status`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `actor_guids[]`：actor_guids 列表。
- `allow_commandlet`：布尔/开关字段；默认值以本命令契约为准。
- `execute`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `timeout_seconds`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- `commandlet_plan`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `status`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
