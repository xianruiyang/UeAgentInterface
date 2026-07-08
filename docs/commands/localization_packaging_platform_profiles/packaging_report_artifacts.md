---
command: packaging_report_artifacts
category: localization_packaging_platform_profiles
title: Packaging
summary: 扫描 archive 目录产物
aliases: []
tags: [localization_packaging_platform_profiles]
status: active
---

# `packaging_report_artifacts`

## LLM Compact

- status: active.
- purpose: 扫描 archive 目录产物
- key_inputs: `archive_dir`, `include_hashes`, `platform`
- key_returns: `artifacts[]`, `missing_expected[]`, `total_size`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `archive_dir`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `include_hashes`：布尔/开关字段；默认值以本命令契约为准。
- `platform`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- `artifacts[]`：artifacts 列表摘要；大列表应写入 output/artifact 或 debug。
- `missing_expected[]`：missing_expected 列表摘要；大列表应写入 output/artifact 或 debug。
- `total_size`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
