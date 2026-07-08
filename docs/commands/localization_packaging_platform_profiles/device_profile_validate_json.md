---
command: device_profile_validate_json
category: localization_packaging_platform_profiles
title: Platform Profiles
summary: 校验 DeviceProfile schema、CVar、base profile 和循环
aliases: []
tags: [localization_packaging_platform_profiles]
status: active
---

# `device_profile_validate_json`

## LLM Compact

- status: active.
- purpose: 校验 DeviceProfile schema、CVar、base profile 和循环
- key_inputs: `json_file`, `json`
- key_returns: `valid`, `issues[]`
- behavior: 只读/计划类命令，不应修改 UE 状态。
- note: 这是只读校验入口；不得把 validate 结果当成已写入状态。
- validation: 检查 `valid/issues/json_issues`；validate 不产生写入副作用。

## Authoring Notes

- 这是只读校验入口；不得把 validate 结果当成已写入状态。

## Parameters

- `json_file`：JSON authoring 文件路径；长 JSON 和可复用 payload 优先走文件。
- `json`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- `valid`：校验是否通过。
- `issues[]`：issues 列表摘要；大列表应写入 output/artifact 或 debug。

## Validation

检查 `valid/issues/json_issues`；validate 不产生写入副作用。
