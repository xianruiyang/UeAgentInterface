---
command: level_actor_snapshot_json
category: level_content
title: 指令
summary: Actor-only 快照兼容别名
aliases: []
tags: [level_content]
status: active
---

# `level_actor_snapshot_json`

## LLM Compact

- status: active.
- purpose: Actor-only 快照兼容别名
- key_inputs: `level_content_snapshot_json`
- key_returns: `actor_only=true`, `level_content_snapshot_json`
- behavior: 只读查询类命令；大结果应写入 `output_file`。
- note: 大对象或长列表优先传 `output_file`，compact 返回只保留摘要和路径。
- note: Level Content JSON 描述场景实例最终态；复杂资产本体仍走 Blueprint/Material/Niagara 等专用 authoring。
- validation: 写操作后用 query/snapshot 读回 label、folder、transform、tags、components/collision；删除先 dry-run 再执行。

## Authoring Notes

- 大对象或长列表优先传 `output_file`，compact 返回只保留摘要和路径。
- Level Content JSON 描述场景实例最终态；复杂资产本体仍走 Blueprint/Material/Niagara 等专用 authoring。

## Parameters

- `level_content_snapshot_json`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- `actor_only=true`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `level_content_snapshot_json`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Validation

写操作后用 query/snapshot 读回 label、folder、transform、tags、components/collision；删除先 dry-run 再执行。
