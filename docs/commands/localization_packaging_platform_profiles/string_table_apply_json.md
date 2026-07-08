---
command: string_table_apply_json
category: localization_packaging_platform_profiles
title: String Table
summary: 创建或更新 `UStringTable` entry/metadata
aliases: []
tags: [localization_packaging_platform_profiles]
status: active
---

# `string_table_apply_json`

## LLM Compact

- status: active.
- purpose: 创建或更新 `UStringTable` entry/metadata
- key_inputs: `asset_path`, `dry_run`, `json_file`, `backup`, `json`, `save_after_apply`
- key_returns: `changed_count`, `created`, `readback`
- behavior: 写入命令；必须明确目标范围、默认值和写后读回验收。
- note: JSON/folder 写入新增内容时先最小 authoring -> apply -> query/export readback -> patch 必要字段 -> 再 apply 验证；不要先找完整模板或源码推字段。
- validation: apply 后立刻 export/query readback，比较关键字段；不能只看 `success=true`。

## Authoring Notes

- JSON/folder 写入新增内容时先最小 authoring -> apply -> query/export readback -> patch 必要字段 -> 再 apply 验证；不要先找完整模板或源码推字段。

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。
- `dry_run`：只计划/报告不写入；删除类命令通常默认 true，执行删除需显式 false。
- `json_file`：JSON authoring 文件路径；长 JSON 和可复用 payload 优先走文件。
- `backup`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `json`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `save_after_apply`：布尔/开关字段；默认值以本命令契约为准。

## Returns

- `changed_count`：数量摘要。
- `created`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `readback`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Validation

apply 后立刻 export/query readback，比较关键字段；不能只看 `success=true`。
