---
command: ik_rig_apply_folder
category: ikrig_ikretargeter
title: IK Rig
summary: 应用 IK Rig 文件夹 JSON
aliases: []
tags: [ikrig_ikretargeter]
status: active
---

# `ik_rig_apply_folder`

## LLM Compact

- status: active.
- purpose: 应用 IK Rig 文件夹 JSON
- key_inputs: `asset_path`, `dry_run`, `folder_path`, `create_if_missing`, `save_after_apply`, `validate_only`
- key_returns: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- behavior: 写入命令；必须明确目标范围、默认值和写后读回验收。
- note: JSON/folder 写入新增内容时先最小 authoring -> apply -> query/export readback -> patch 必要字段 -> 再 apply 验证；不要先找完整模板或源码推字段。
- validation: apply 后立刻 export/query readback，比较关键字段；不能只看 `success=true`。

## Folder Authoring Contract

- Folder workflow properties authoring is grouped Details only: edit `details/index.json` plus referenced `details/<group>.json`, or domain-specific `*_details` references that point to grouped Details files
- Do not author editable properties through root `properties.json`, inline `properties[]`, `*_properties[]`, `raw_properties.json`, or `readonly_properties.json`
- `raw_properties`, `readonly_properties`, `missing_properties`, and `unsupported_properties` are export, diagnostic, or readback fields only, not apply input
- `uai.uobject_properties.v1.properties` is an internal loaded schema after grouped Details are read, not the folder authoring file shape

## Authoring Notes
- JSON/folder 写入新增内容时先最小 authoring -> apply -> query/export readback -> patch 必要字段 -> 再 apply 验证；不要先找完整模板或源码推字段。

## Parameters

- `asset_path`：UE 资产路径；支持 `/Game/...`，部分命令也支持 `/Engine/...`。
- `dry_run`：只计划/报告不写入；删除类命令通常默认 true，执行删除需显式 false。
- `folder_path`：Outliner folder 或资产 folder；具体含义以命令所在领域为准。
- `create_if_missing`：按命令实现读取/写入的字段；写入后用对应 readback 验证。
- `save_after_apply`：布尔/开关字段；默认值以本命令契约为准。
- `validate_only`：按命令实现读取/写入的字段；写入后用对应 readback 验证。

## Returns

- 未在旧分册命令表中列出；以实现返回和 readback 为准。

## Validation

apply 后立刻 export/query readback，比较关键字段；不能只看 `success=true`。
