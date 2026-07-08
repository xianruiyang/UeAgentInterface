---
command: level_topology_set_actor_assignments
category: level_topology_streaming_world_partition_data_layer_hlod
title: 拓扑编排
summary: 一次性设置 Actor 的子关卡、DataLayer、HLOD、WP 相关分配
aliases: []
tags: [level_topology_streaming_world_partition_data_layer_hlod]
status: active
---

# `level_topology_set_actor_assignments`

## LLM Compact

- status: active.
- purpose: 一次性设置 Actor 的子关卡、DataLayer、HLOD、WP 相关分配
- key_inputs: `actor_ids`, `data_layers`, `hlod_layer`, `is_spatially_loaded`, `level_package`, `runtime_grid`
- key_returns: `changed`, `results[]`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `actor_ids`：稳定 id 列表。
- `data_layers`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `hlod_layer`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `is_spatially_loaded`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `level_package`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `runtime_grid`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- `changed`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `results[]`：results 列表摘要；大列表应写入 output/artifact 或 debug。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
