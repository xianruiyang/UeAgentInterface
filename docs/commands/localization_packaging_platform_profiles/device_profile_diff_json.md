---
command: device_profile_diff_json
category: localization_packaging_platform_profiles
title: Platform Profiles
summary: 比较目标 DeviceProfile 与当前配置
aliases: []
tags: [localization_packaging_platform_profiles]
status: active
---

# `device_profile_diff_json`

## LLM Compact

- status: active.
- purpose: 比较目标 DeviceProfile 与当前配置
- key_inputs: `json_file`, `json`
- key_returns: `added_cvars[]`, `changed_cvars[]`, `removed_cvars[]`
- behavior: 只读/计划类命令，不应修改 UE 状态。
- validation: 读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。

## Authoring Notes

- 按本命令参数、返回字段和写后读回规则执行。

## Parameters

- `json_file`：JSON authoring 文件路径；长 JSON 和可复用 payload 优先走文件。
- `json`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- `added_cvars[]`：added_cvars 列表摘要；大列表应写入 output/artifact 或 debug。
- `changed_cvars[]`：changed_cvars 列表摘要；大列表应写入 output/artifact 或 debug。
- `removed_cvars[]`：removed_cvars 列表摘要；大列表应写入 output/artifact 或 debug。

## Validation

读取 `CommandResult.data` 的关键返回字段；失败时只展开 `debug/envelope` 定位原因。
