---
command: hlod_assign_actors
category: level_topology_streaming_world_partition_data_layer_hlod
title: HLOD
summary: 给 Actor 设置 HLOD Layer、Runtime Grid、Spatially Loaded
aliases: []
tags: [level_topology_streaming_world_partition_data_layer_hlod]
status: active
---

# `hlod_assign_actors`

## LLM Compact

- status: active.
- purpose: 给 Actor 设置 HLOD Layer、Runtime Grid、Spatially Loaded
- key_inputs: `folder_root`, `actor_ids`, `hlod_layer`, `is_spatially_loaded`, `runtime_grid`
- key_returns: `changed_count`, `matched_count`, `results[]`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `folder_root`：托管范围根 folder；场景清理时是 Outliner folder，不是 `/Game` 资产目录。
- `actor_ids`：稳定 id 列表。
- `hlod_layer`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `is_spatially_loaded`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `runtime_grid`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- `changed_count`：数量摘要。
- `matched_count`：数量摘要。
- `results[]`：results 列表摘要；大列表应写入 output/artifact 或 debug。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
