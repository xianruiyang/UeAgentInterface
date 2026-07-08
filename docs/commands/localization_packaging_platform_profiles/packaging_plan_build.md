---
command: packaging_plan_build
category: localization_packaging_platform_profiles
title: Packaging
summary: 生成 UAT BuildCookRun 命令计划
aliases: []
tags: [localization_packaging_platform_profiles]
status: active
---

# `packaging_plan_build`

## LLM Compact

- status: active.
- purpose: 生成 UAT BuildCookRun 命令计划
- key_inputs: `json_file`, `archive`, `configuration`, `cook`, `json`, `package`, `platform`, `stage`
- key_returns: `artifacts_expected[]`, `command_line`, `uat_args[]`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `json_file`：JSON authoring 文件路径；长 JSON 和可复用 payload 优先走文件。
- `archive`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `configuration`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `cook`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `json`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `package`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `platform`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `stage`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- `artifacts_expected[]`：artifacts_expected 列表摘要；大列表应写入 output/artifact 或 debug。
- `command_line`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `uat_args[]`：uat_args 列表摘要；大列表应写入 output/artifact 或 debug。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
