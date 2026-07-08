---
command: data_layer_set_actor_membership
category: level_topology_streaming_world_partition_data_layer_hlod
title: DataLayer
summary: 设置 Actor 与 DataLayer 的成员关系
aliases: []
tags: [level_topology_streaming_world_partition_data_layer_hlod]
status: active
---

# `data_layer_set_actor_membership`

## LLM Compact

- status: active.
- purpose: 设置 Actor 与 DataLayer 的成员关系
- key_inputs: `folder_root`, `actor_ids`, `layers[]`
- key_returns: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `folder_root`：托管范围根 folder；场景清理时是 Outliner folder，不是 `/Game` 资产目录。
- `actor_ids`：稳定 id 列表。
- `layers[]`：layers 列表。

## Returns

- 未在旧分册命令表中列出；以实现返回和 readback 为准。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
