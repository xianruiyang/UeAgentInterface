---
command: data_layer_create_asset
category: level_topology_streaming_world_partition_data_layer_hlod
title: DataLayer
summary: 创建或更新 `UDataLayerAsset`
aliases: []
tags: [level_topology_streaming_world_partition_data_layer_hlod]
status: active
---

# `data_layer_create_asset`

## LLM Compact

- status: active.
- purpose: 创建或更新 `UDataLayerAsset`
- key_inputs: `asset_path`
- key_returns: `、`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。

## Returns

- `、`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
