---
command: hlod_layer_export_json
category: level_topology_streaming_world_partition_data_layer_hlod
title: HLOD
summary: 导出 HLOD Layer JSON
aliases: []
tags: [level_topology_streaming_world_partition_data_layer_hlod]
status: active
---

# `hlod_layer_export_json`

## LLM Compact

- status: active.
- purpose: 导出 HLOD Layer JSON
- key_inputs: `asset_path`, `output_file`
- key_returns: `cell_size`, `layer_type`, `loading_range`, `parent_layer`
- note: 大对象或长列表优先传 `output_file`，compact 返回只保留摘要和路径。
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 大对象或长列表优先传 `output_file`，compact 返回只保留摘要和路径。

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。
- `output_file`：输出 artifact 路径；大结果应写文件，不要内联到 compact data。

## Returns

- `cell_size`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `layer_type`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `loading_range`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `parent_layer`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
