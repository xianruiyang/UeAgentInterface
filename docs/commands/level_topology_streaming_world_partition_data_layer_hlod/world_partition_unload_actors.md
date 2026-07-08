---
command: world_partition_unload_actors
category: level_topology_streaming_world_partition_data_layer_hlod
title: World Partition
summary: 卸载 actor list loader
aliases: []
tags: [level_topology_streaming_world_partition_data_layer_hlod]
status: active
---

# `world_partition_unload_actors`

## LLM Compact

- status: active.
- purpose: 卸载 actor list loader
- key_inputs: `loader_id`
- key_returns: `unloaded`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `loader_id`：稳定 id，用于精确定位或后续操作。

## Returns

- `unloaded`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
