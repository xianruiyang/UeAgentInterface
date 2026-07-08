---
command: level_streaming_remove
category: level_topology_streaming_world_partition_data_layer_hlod
title: Level Streaming
summary: 从当前 World 移除 streaming level
aliases: []
tags: [level_topology_streaming_world_partition_data_layer_hlod]
status: active
---

# `level_streaming_remove`

## LLM Compact

- status: active.
- purpose: 从当前 World 移除 streaming level
- key_inputs: `clear_selection`, `delete_asset`, `level_package`, `reset_transaction_buffer`
- key_returns: `delete_asset`, `level_package`, `removed`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `clear_selection`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `delete_asset`：资产引用；通常传 UE 资产路径。
- `level_package`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `reset_transaction_buffer`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- `level_package`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `removed`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `delete_asset`：资产引用；通常传 UE 资产路径。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
