---
command: hlod_validate_build
category: level_topology_streaming_world_partition_data_layer_hlod
title: HLOD
summary: 读取当前场景 HLOD actor 数量和问题列表
aliases: []
tags: [level_topology_streaming_world_partition_data_layer_hlod]
status: active
---

# `hlod_validate_build`

## LLM Compact

- status: active.
- purpose: 读取当前场景 HLOD actor 数量和问题列表
- key_inputs: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- key_returns: `hlod_actor_count`, `issues[]`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- 未在旧分册命令表中列出；以实现 schema/readback 为准。

## Returns

- `hlod_actor_count`：数量摘要。
- `issues[]`：issues 列表摘要；大列表应写入 output/artifact 或 debug。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
