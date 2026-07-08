---
command: texture_graph_validate_folder
category: texture_render_target_media
title: SVT / TextureGraph / 2D 派生资产
summary: Texture Graph folder workflow、bake 和运行探针。
aliases: []
tags: [texture_render_target_media]
status: active
---

# `texture_graph_validate_folder`

## LLM Compact

- status: active.
- purpose: Texture Graph folder workflow、bake 和运行探针。
- key_inputs: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- key_returns: 无显式字段；使用 `**params` 传入并按单命令契约确认。
- note: 这是只读校验入口；不得把 validate 结果当成已写入状态。
- validation: 检查 `valid/issues/json_issues`；validate 不产生写入副作用。

## Folder Authoring Contract

- Folder workflow properties authoring is grouped Details only: edit `details/index.json` plus referenced `details/<group>.json`, or domain-specific `*_details` references that point to grouped Details files
- Do not author editable properties through root `properties.json`, inline `properties[]`, `*_properties[]`, `raw_properties.json`, or `readonly_properties.json`
- `raw_properties`, `readonly_properties`, `missing_properties`, and `unsupported_properties` are export, diagnostic, or readback fields only, not apply input
- `uai.uobject_properties.v1.properties` is an internal loaded schema after grouped Details are read, not the folder authoring file shape

## Authoring Notes
- 这是只读校验入口；不得把 validate 结果当成已写入状态。

## Parameters

- 未在旧分册命令表中列出；以实现 schema/readback 为准。

## Returns

- 未在旧分册命令表中列出；以实现返回和 readback 为准。

## Validation

检查 `valid/issues/json_issues`；validate 不产生写入副作用。
