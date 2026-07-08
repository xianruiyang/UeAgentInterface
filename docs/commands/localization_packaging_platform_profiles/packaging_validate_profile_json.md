---
command: packaging_validate_profile_json
category: localization_packaging_platform_profiles
title: Packaging
summary: 校验平台、配置、map、UAT 和 archive 目录
aliases: []
tags: [localization_packaging_platform_profiles]
status: active
---

# `packaging_validate_profile_json`

## LLM Compact

- status: active.
- purpose: 校验平台、配置、map、UAT 和 archive 目录
- key_inputs: `json_file`, `json`
- key_returns: `valid`, `estimated_actions[]`, `issues[]`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `json_file`：JSON authoring 文件路径；长 JSON 和可复用 payload 优先走文件。
- `json`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- `valid`：校验是否通过。
- `estimated_actions[]`：estimated_actions 列表摘要；大列表应写入 output/artifact 或 debug。
- `issues[]`：issues 列表摘要；大列表应写入 output/artifact 或 debug。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
