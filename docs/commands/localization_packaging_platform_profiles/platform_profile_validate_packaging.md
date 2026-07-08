---
command: platform_profile_validate_packaging
category: localization_packaging_platform_profiles
title: Platform Profiles
summary: 联合校验 packaging profile、platform 和 device profile
aliases: []
tags: [localization_packaging_platform_profiles]
status: active
---

# `platform_profile_validate_packaging`

## LLM Compact

- status: active.
- purpose: 联合校验 packaging profile、platform 和 device profile
- key_inputs: `device_profile`, `packaging_json`, `platform`
- key_returns: `valid`, `issues[]`, `warnings[]`
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `device_profile`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `packaging_json`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `platform`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- `valid`：校验是否通过。
- `issues[]`：issues 列表摘要；大列表应写入 output/artifact 或 debug。
- `warnings[]`：warnings 列表摘要；大列表应写入 output/artifact 或 debug。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
