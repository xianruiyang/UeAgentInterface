---
command: hlod_layer_apply_json
category: level_topology_streaming_world_partition_data_layer_hlod
title: HLOD
summary: 按 JSON 创建或回写 HLOD Layer
aliases: []
tags: [level_topology_streaming_world_partition_data_layer_hlod]
status: active
---

# `hlod_layer_apply_json`

## LLM Compact

- status: active.
- purpose: 按 JSON 创建或回写 HLOD Layer
- key_inputs: `asset_path`, `json_file`, `json`, `overwrite_policy`, `save_after_apply`
- key_returns: `property_results[]`, `status`
- behavior: 写入命令；必须明确目标范围、默认值和写后读回验收。
- note: JSON/folder 写入新增内容时先最小 authoring -> apply -> query/export readback -> patch 必要字段 -> 再 apply 验证；不要先找完整模板或源码推字段。
- validation: apply 后立刻 export/query readback，比较关键字段；不能只看 `success=true`。

## Authoring Notes

- JSON/folder 写入新增内容时先最小 authoring -> apply -> query/export readback -> patch 必要字段 -> 再 apply 验证；不要先找完整模板或源码推字段。

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。
- `json_file`：JSON authoring 文件路径；长 JSON 和可复用 payload 优先走文件。
- `json`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `overwrite_policy`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `save_after_apply`：布尔/开关字段；默认值以本命令契约为准。

## Returns

- `property_results[]`：property_results 列表摘要；大列表应写入 output/artifact 或 debug。
- `status`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Validation

apply 后立刻 export/query readback，比较关键字段；不能只看 `success=true`。
