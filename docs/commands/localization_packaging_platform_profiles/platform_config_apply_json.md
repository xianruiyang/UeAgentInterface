---
command: platform_config_apply_json
category: localization_packaging_platform_profiles
title: Platform Profiles
summary: 写入受控平台 config patch
aliases: []
tags: [localization_packaging_platform_profiles]
status: active
---

# `platform_config_apply_json`

## LLM Compact

- status: active.
- purpose: 写入受控平台 config patch
- key_inputs: `dry_run`, `json_file`, `backup`, `json`
- key_returns: `backup_paths[]`, `changes[]`
- behavior: 写入命令；必须明确目标范围、默认值和写后读回验收。
- note: JSON/folder 写入新增内容时先最小 authoring -> apply -> query/export readback -> patch 必要字段 -> 再 apply 验证；不要先找完整模板或源码推字段。
- validation: apply 后立刻 export/query readback，比较关键字段；不能只看 `success=true`。

## Authoring Notes

- JSON/folder 写入新增内容时先最小 authoring -> apply -> query/export readback -> patch 必要字段 -> 再 apply 验证；不要先找完整模板或源码推字段。

## Parameters

- `dry_run`：只计划/报告不写入；删除类命令通常默认 true，执行删除需显式 false。
- `json_file`：JSON authoring 文件路径；长 JSON 和可复用 payload 优先走文件。
- `backup`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `json`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- `backup_paths[]`：backup_paths 列表摘要；大列表应写入 output/artifact 或 debug。
- `changes[]`：changes 列表摘要；大列表应写入 output/artifact 或 debug。

## Validation

apply 后立刻 export/query readback，比较关键字段；不能只看 `success=true`。
