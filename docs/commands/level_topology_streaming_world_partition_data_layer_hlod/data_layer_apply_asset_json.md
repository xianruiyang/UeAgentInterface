---
command: data_layer_apply_asset_json
category: level_topology_streaming_world_partition_data_layer_hlod
title: DataLayer
summary: 按 JSON 创建或更新 DataLayerAsset
aliases: []
tags: [level_topology_streaming_world_partition_data_layer_hlod]
status: active
---

# `data_layer_apply_asset_json`

## LLM Compact

- status: active.
- purpose: 按 JSON 创建或更新 DataLayerAsset
- key_inputs: `asset_path`, `json_file`, `json`
- key_returns: `data_layer_create_asset`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。
- `json_file`：JSON authoring 文件路径；长 JSON 和可复用 payload 优先走文件。
- `json`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- `data_layer_create_asset`：资产引用；通常传 UE 资产路径。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
