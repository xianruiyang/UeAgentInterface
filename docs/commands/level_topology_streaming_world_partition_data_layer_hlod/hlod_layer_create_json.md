---
command: hlod_layer_create_json
category: level_topology_streaming_world_partition_data_layer_hlod
title: HLOD
summary: 创建或更新 HLOD Layer 资产
aliases: []
tags: [level_topology_streaming_world_partition_data_layer_hlod]
status: active
---

# `hlod_layer_create_json`

## LLM Compact

- status: active.
- purpose: 创建或更新 HLOD Layer 资产
- key_inputs: `asset_path`, `json_file`, `json`, `overwrite_policy`, `save_after_apply`
- key_returns: `asset_path`, `layer_type`, `status`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。
- `json_file`：JSON authoring 文件路径；长 JSON 和可复用 payload 优先走文件。
- `json`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `overwrite_policy`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `save_after_apply`：布尔/开关字段；默认值以本命令契约为准。

## Returns

- `asset_path`：路径字段；通常为 UE 资产路径、对象路径或输出文件路径，具体语义按命令上下文。
- `layer_type`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `status`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
