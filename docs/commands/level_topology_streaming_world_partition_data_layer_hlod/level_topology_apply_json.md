---
command: level_topology_apply_json
category: level_topology_streaming_world_partition_data_layer_hlod
title: 拓扑编排
summary: 批量应用 streaming level、DataLayer 和 Actor 拓扑关系
aliases: []
tags: [level_topology_streaming_world_partition_data_layer_hlod]
status: active
---

# `level_topology_apply_json`

## LLM Compact

- status: active.
- purpose: 批量应用 streaming level、DataLayer 和 Actor 拓扑关系
- key_inputs: `dry_run`, `json_file`, `json`
- key_returns: `changed`, `results[]`, `status`
- behavior: 写入命令；必须明确目标范围、默认值和写后读回验收。
- note: JSON/folder 写入新增内容时先最小 authoring -> apply -> query/export readback -> patch 必要字段 -> 再 apply 验证；不要先找完整模板或源码推字段。
- validation: apply 后立刻 export/query readback，比较关键字段；不能只看 `success=true`。

## Authoring Notes

- JSON/folder 写入新增内容时先最小 authoring -> apply -> query/export readback -> patch 必要字段 -> 再 apply 验证；不要先找完整模板或源码推字段。

## Parameters

- `dry_run`：只计划/报告不写入；删除类命令通常默认 true，执行删除需显式 false。
- `json_file`：JSON authoring 文件路径；长 JSON 和可复用 payload 优先走文件。
- `json`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- `changed`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `results[]`：results 列表摘要；大列表应写入 output/artifact 或 debug。
- `status`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Validation

apply 后立刻 export/query readback，比较关键字段；不能只看 `success=true`。
