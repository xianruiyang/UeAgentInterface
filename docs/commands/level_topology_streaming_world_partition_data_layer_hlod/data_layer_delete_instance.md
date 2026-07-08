---
command: data_layer_delete_instance
category: level_topology_streaming_world_partition_data_layer_hlod
title: DataLayer
summary: 删除当前 World 的 DataLayer instance
aliases: []
tags: [level_topology_streaming_world_partition_data_layer_hlod]
status: active
---

# `data_layer_delete_instance`

## LLM Compact

- status: active.
- purpose: 删除当前 World 的 DataLayer instance
- key_inputs: `transaction`, `layer`
- key_returns: `deleted`, `layer`
- behavior: 破坏性命令；先 dry-run/plan，确认候选、阻断项和保护规则后再执行。
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `transaction`：是否使用 UE transaction 包裹写入。
- `layer`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- `deleted`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `layer`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
