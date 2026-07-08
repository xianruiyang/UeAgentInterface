---
command: level_streaming_validate
category: level_topology_streaming_world_partition_data_layer_hlod
title: Level Streaming
summary: 校验子关卡存在、加载/可见状态、Actor 所属关卡
aliases: []
tags: [level_topology_streaming_world_partition_data_layer_hlod]
status: active
---

# `level_streaming_validate`

## LLM Compact

- status: active.
- purpose: 校验子关卡存在、加载/可见状态、Actor 所属关卡
- key_inputs: `actor_ids`, `expected_loaded`, `expected_visible`, `level_package`
- key_returns: `issues[]`, `status`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `actor_ids`：稳定 id 列表。
- `expected_loaded`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `expected_visible`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `level_package`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- `issues[]`：issues 列表摘要；大列表应写入 output/artifact 或 debug。
- `status`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
