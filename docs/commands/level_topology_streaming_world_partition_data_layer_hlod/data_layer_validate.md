---
command: data_layer_validate
category: level_topology_streaming_world_partition_data_layer_hlod
title: DataLayer
summary: 校验 DataLayer 与 Actor 归属
aliases: []
tags: [level_topology_streaming_world_partition_data_layer_hlod]
status: active
---

# `data_layer_validate`

## LLM Compact

- status: active.
- purpose: 校验 DataLayer 与 Actor 归属
- key_inputs: `actor_ids`, `expect_actor_membership[]`, `layers[]`
- key_returns: `issues[]`, `status`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `actor_ids`：稳定 id 列表。
- `expect_actor_membership[]`：expect_actor_membership 列表。
- `layers[]`：layers 列表。

## Returns

- `issues[]`：issues 列表摘要；大列表应写入 output/artifact 或 debug。
- `status`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
