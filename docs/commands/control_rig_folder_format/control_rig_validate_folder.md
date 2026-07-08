---
command: control_rig_validate_folder
category: control_rig_folder_format
title: Control Rig 命令
summary: 只读校验 folder JSON
aliases: []
tags: [control_rig_folder_format]
status: active
---

# `control_rig_validate_folder`

## LLM Compact

- status: active.
- purpose: 只读校验 folder JSON
- key_inputs: `asset_path`, `folder_path`, `create_if_missing`, `require_animation_editing_ready`
- key_returns: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- note: 这是只读校验入口；不得把 validate 结果当成已写入状态。
- note: folder authoring 以导出/最小 authoring/readback 为主；新增节点、组件或模块先用 catalog/seed，再按读回修补。
- validation: 检查 `valid/issues/json_issues`；validate 不产生写入副作用。

## Folder Authoring Contract

- Folder workflow properties authoring is grouped Details only: edit `details/index.json` plus referenced `details/<group>.json`, or domain-specific `*_details` references that point to grouped Details files
- Do not author editable properties through root `properties.json`, inline `properties[]`, `*_properties[]`, `raw_properties.json`, or `readonly_properties.json`
- `raw_properties`, `readonly_properties`, `missing_properties`, and `unsupported_properties` are export, diagnostic, or readback fields only, not apply input
- `uai.uobject_properties.v1.properties` is an internal loaded schema after grouped Details are read, not the folder authoring file shape

## Authoring Notes
- 这是只读校验入口；不得把 validate 结果当成已写入状态。
- folder authoring 以导出/最小 authoring/readback 为主；新增节点、组件或模块先用 catalog/seed，再按读回修补。

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。
- `folder_path`：Outliner folder 或资产 folder；具体含义以命令所在领域为准。
- `create_if_missing`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `require_animation_editing_ready`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- 未在旧分册命令表中列出；以实现返回和 readback 为准。

## Validation

检查 `valid/issues/json_issues`；validate 不产生写入副作用。
