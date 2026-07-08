---
command: packaging_clean_artifacts
category: localization_packaging_platform_profiles
title: Packaging
summary: 清理 Project/Saved 下 UAI 托管打包输出
aliases: []
tags: [localization_packaging_platform_profiles]
status: active
---

# `packaging_clean_artifacts`

## LLM Compact

- status: active.
- purpose: 清理 Project/Saved 下 UAI 托管打包输出
- key_inputs: `dry_run`, `archive_dir`, `max_delete_bytes`
- key_returns: `deleted[]`, `freed_bytes`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `dry_run`：只计划/报告不写入；删除类命令通常默认 true，执行删除需显式 false。
- `archive_dir`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `max_delete_bytes`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- `deleted[]`：deleted 列表摘要；大列表应写入 output/artifact 或 debug。
- `freed_bytes`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
